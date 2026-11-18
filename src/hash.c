/* Hash table.

   This data structure is thoroughly documented in the Tour of
   Pintos for Project 3.

   See hash.h for basic information. */

#include "hash.h"
#include <assert.h>
#include <stdlib.h>

#define list_elem_to_hash_elem(LIST_ELEM)                       \
	list_entry(LIST_ELEM, struct hash_elem, list_elem)

static void clear_bucket (struct hash *, struct list *, hash_action_func *);
static struct list *find_bucket (struct hash *, struct hash_elem *);
static struct hash_elem *find_elem (struct hash *, struct list *,
		struct hash_elem *);
static struct hash_elem *find_any (struct hash *, struct hash_elem *);
static void insert_elem (struct hash *, struct list *, struct hash_elem *);
static void remove_elem (struct hash *, struct hash_elem *);
static void rehash (struct hash *);
static void rehash_step (struct hash *);
static void rehash_finish (struct hash *);
static void rehash_move_bucket (struct hash *, struct list *);
static void rehash_end (struct hash *);

/* Initializes hash table H to compute hash values using HASH and
   compare hash elements using LESS, given auxiliary data AUX. */
bool
hash_init (struct hash *h,
		hash_hash_func *hash, hash_less_func *less, void *aux) {
	h->elem_cnt = 0;
	h->bucket_cnt = 4;
	h->buckets = malloc (sizeof *h->buckets * h->bucket_cnt);
	h->old_buckets = NULL;
	h->old_bucket_cnt = 0;
	h->rehash_idx = 0;
	h->hash = hash;
	h->less = less;
	h->aux = aux;

	if (h->buckets != NULL) {
		hash_clear (h, NULL);
		return true;
	} else
		return false;
}

/* Removes all the elements from H.

   If DESTRUCTOR is non-null, then it is called for each element
   in the hash.  DESTRUCTOR may, if appropriate, deallocate the
   memory used by the hash element.  However, modifying hash
   table H while hash_clear() is running, using any of the
   functions hash_clear(), hash_destroy(), hash_insert(),
   hash_replace(), or hash_delete(), yields undefined behavior,
   whether done in DESTRUCTOR or elsewhere. */
void
hash_clear (struct hash *h, hash_action_func *destructor) {
	size_t i;

	for (i = 0; i < h->bucket_cnt; i++)
		clear_bucket (h, &h->buckets[i], destructor);

	/* mini-redis perf: 옮기는 중이면 아직 옮기지 않은 옛 버킷의 entry도
	   지우고, 옛 배열을 버립니다. */
	if (h->old_buckets != NULL) {
		for (i = h->rehash_idx; i < h->old_bucket_cnt; i++)
			clear_bucket (h, &h->old_buckets[i], destructor);
		rehash_end (h);
	}

	h->elem_cnt = 0;
}

/* Destroys hash table H.

   If DESTRUCTOR is non-null, then it is first called for each
   element in the hash.  DESTRUCTOR may, if appropriate,
   deallocate the memory used by the hash element.  However,
   modifying hash table H while hash_clear() is running, using
   any of the functions hash_clear(), hash_destroy(),
   hash_insert(), hash_replace(), or hash_delete(), yields
   undefined behavior, whether done in DESTRUCTOR or
   elsewhere. */
void
hash_destroy (struct hash *h, hash_action_func *destructor) {
	if (destructor != NULL)
		hash_clear (h, destructor);
	free (h->buckets);
	free (h->old_buckets);
}

/* Inserts NEW into hash table H and returns a null pointer, if
   no equal element is already in the table.
   If an equal element is already in the table, returns it
   without inserting NEW. */
struct hash_elem *
hash_insert (struct hash *h, struct hash_elem *new) {
	struct hash_elem *old;

	rehash_step (h);
	old = find_any (h, new);
	if (old == NULL)
		insert_elem (h, find_bucket (h, new), new);

	rehash (h);

	return old;
}

/* Inserts NEW into hash table H, replacing any equal element
   already in the table, which is returned. */
struct hash_elem *
hash_replace (struct hash *h, struct hash_elem *new) {
	struct hash_elem *old;

	rehash_step (h);
	old = find_any (h, new);
	if (old != NULL)
		remove_elem (h, old);
	insert_elem (h, find_bucket (h, new), new);

	rehash (h);

	return old;
}

/* Finds and returns an element equal to E in hash table H, or a
   null pointer if no equal element exists in the table.

   mini-redis perf: 조회도 옮기기를 한 걸음 진행합니다. 읽기만 오는 동안에도
   옮기기가 끝나게 하기 위해서입니다. entry의 메모리는 움직이지 않고 버킷
   사이에서 연결만 바뀌므로, 찾은 원소를 가리키는 포인터는 그대로 쓸 수
   있습니다. */
struct hash_elem *
hash_find (struct hash *h, struct hash_elem *e) {
	rehash_step (h);
	return find_any (h, e);
}

/* Finds, removes, and returns an element equal to E in hash
   table H.  Returns a null pointer if no equal element existed
   in the table.

   If the elements of the hash table are dynamically allocated,
   or own resources that are, then it is the caller's
   responsibility to deallocate them. */
struct hash_elem *
hash_delete (struct hash *h, struct hash_elem *e) {
	struct hash_elem *found;

	rehash_step (h);
	found = find_any (h, e);
	if (found != NULL) {
		remove_elem (h, found);
		rehash (h);
	}
	return found;
}

/* Calls ACTION for each element in hash table H in arbitrary
   order.
   Modifying hash table H while hash_apply() is running, using
   any of the functions hash_clear(), hash_destroy(),
   hash_insert(), hash_replace(), or hash_delete(), yields
   undefined behavior, whether done from ACTION or elsewhere. */
void
hash_apply (struct hash *h, hash_action_func *action) {
	size_t i;

	assert (action != NULL);

	/* mini-redis perf: 훑기는 새 배열만 돌므로 남은 옮기기를 먼저 끝냅니다.
	   훑기 자체가 모든 원소를 도는 일이라 비용의 차수는 같습니다. */
	rehash_finish (h);

	for (i = 0; i < h->bucket_cnt; i++) {
		struct list *bucket = &h->buckets[i];
		struct list_elem *elem, *next;

		for (elem = list_begin (bucket); elem != list_end (bucket); elem = next) {
			next = list_next (elem);
			action (list_elem_to_hash_elem (elem), h->aux);
		}
	}
}

/* Initializes I for iterating hash table H.

   Iteration idiom:

   struct hash_iterator i;

   hash_first (&i, h);
   while (hash_next (&i))
   {
   struct foo *f = hash_entry (hash_cur (&i), struct foo, elem);
   ...do something with f...
   }

   Modifying hash table H during iteration, using any of the
   functions hash_clear(), hash_destroy(), hash_insert(),
   hash_replace(), or hash_delete(), invalidates all
   iterators. */
void
hash_first (struct hash_iterator *i, struct hash *h) {
	assert (i != NULL);
	assert (h != NULL);

	/* mini-redis perf: hash_apply와 같은 이유로 남은 옮기기를 먼저 끝냅니다. */
	rehash_finish (h);

	i->hash = h;
	i->bucket = i->hash->buckets;
	i->elem = list_elem_to_hash_elem (list_head (i->bucket));
}

/* Advances I to the next element in the hash table and returns
   it.  Returns a null pointer if no elements are left.  Elements
   are returned in arbitrary order.

   Modifying a hash table H during iteration, using any of the
   functions hash_clear(), hash_destroy(), hash_insert(),
   hash_replace(), or hash_delete(), invalidates all
   iterators. */
struct hash_elem *
hash_next (struct hash_iterator *i) {
	assert (i != NULL);

	i->elem = list_elem_to_hash_elem (list_next (&i->elem->list_elem));
	while (i->elem == list_elem_to_hash_elem (list_end (i->bucket))) {
		if (++i->bucket >= i->hash->buckets + i->hash->bucket_cnt) {
			i->elem = NULL;
			break;
		}
		i->elem = list_elem_to_hash_elem (list_begin (i->bucket));
	}

	return i->elem;
}

/* Returns the current element in the hash table iteration, or a
   null pointer at the end of the table.  Undefined behavior
   after calling hash_first() but before hash_next(). */
struct hash_elem *
hash_cur (struct hash_iterator *i) {
	return i->elem;
}

/* Returns the number of elements in H. */
size_t
hash_size (struct hash *h) {
	return h->elem_cnt;
}

/* Returns true if H contains no elements, false otherwise. */
bool
hash_empty (struct hash *h) {
	return h->elem_cnt == 0;
}

/* mini-redis perf: 버킷 수를 바꾸며 entry를 옮기는 중이면 true를
   돌려줍니다. 테스트가 옮기는 도중의 상태를 실제로 지났는지 확인할 때
   씁니다. */
bool
hash_is_rehashing (struct hash *h) {
	return h->old_buckets != NULL;
}

/* Fowler-Noll-Vo hash constants, for 32-bit word sizes. */
#define FNV_64_PRIME 0x00000100000001B3UL
#define FNV_64_BASIS 0xcbf29ce484222325UL

/* Returns a hash of the SIZE bytes in BUF. */
uint64_t
hash_bytes (const void *buf_, size_t size) {
	/* Fowler-Noll-Vo 32-bit hash, for bytes. */
	const unsigned char *buf = buf_;
	uint64_t hash;

	assert (buf != NULL);

	hash = FNV_64_BASIS;
	while (size-- > 0)
		hash = (hash * FNV_64_PRIME) ^ *buf++;

	return hash;
}

/* Returns a hash of string S. */
uint64_t
hash_string (const char *s_) {
	const unsigned char *s = (const unsigned char *) s_;
	uint64_t hash;

	assert (s != NULL);

	hash = FNV_64_BASIS;
	while (*s != '\0')
		hash = (hash * FNV_64_PRIME) ^ *s++;

	return hash;
}

/* Returns a hash of integer I. */
uint64_t
hash_int (int i) {
	return hash_bytes (&i, sizeof i);
}

/* Removes every element of BUCKET in H, calling DESTRUCTOR on
   each if it is non-null, and leaves BUCKET empty. */
static void
clear_bucket (struct hash *h, struct list *bucket,
		hash_action_func *destructor) {
	if (destructor != NULL)
		while (!list_empty (bucket)) {
			struct list_elem *list_elem = list_pop_front (bucket);
			struct hash_elem *hash_elem = list_elem_to_hash_elem (list_elem);
			destructor (hash_elem, h->aux);
		}

	list_init (bucket);
}

/* Returns the bucket in H that E belongs in. */
static struct list *
find_bucket (struct hash *h, struct hash_elem *e) {
	size_t bucket_idx = h->hash (e, h->aux) & (h->bucket_cnt - 1);
	return &h->buckets[bucket_idx];
}

/* Searches BUCKET in H for a hash element equal to E.  Returns
   it if found or a null pointer otherwise. */
static struct hash_elem *
find_elem (struct hash *h, struct list *bucket, struct hash_elem *e) {
	struct list_elem *i;

	for (i = list_begin (bucket); i != list_end (bucket); i = list_next (i)) {
		struct hash_elem *hi = list_elem_to_hash_elem (i);
		if (!h->less (hi, e, h->aux) && !h->less (e, hi, h->aux))
			return hi;
	}
	return NULL;
}

/* mini-redis perf: H에서 E와 같은 원소를 찾습니다. 옮기는 중이면 원소가
   아직 옛 배열에 있을 수 있으므로, 새 배열에서 찾지 못했을 때 옛 배열의
   버킷도 봅니다. rehash_idx보다 앞의 옛 버킷은 이미 비었으므로 보지
   않습니다. */
static struct hash_elem *
find_any (struct hash *h, struct hash_elem *e) {
	struct hash_elem *found = find_elem (h, find_bucket (h, e), e);
	size_t old_idx;

	if (found != NULL || h->old_buckets == NULL)
		return found;
	old_idx = h->hash (e, h->aux) & (h->old_bucket_cnt - 1);
	if (old_idx < h->rehash_idx)
		return NULL;
	return find_elem (h, &h->old_buckets[old_idx], e);
}

/* Returns X with its lowest-order bit set to 1 turned off. */
static inline size_t
turn_off_least_1bit (size_t x) {
	return x & (x - 1);
}

/* Returns true if X is a power of 2, otherwise false. */
static inline size_t
is_power_of_2 (size_t x) {
	return x != 0 && turn_off_least_1bit (x) == 0;
}

/* Element per bucket ratios. */
#define MIN_ELEMS_PER_BUCKET  1 /* Elems/bucket < 1: reduce # of buckets. */
#define BEST_ELEMS_PER_BUCKET 2 /* Ideal elems/bucket. */
#define MAX_ELEMS_PER_BUCKET  4 /* Elems/bucket > 4: increase # of buckets. */

/* mini-redis perf: 한 걸음에 비어 있지 않은 옛 버킷 하나를 옮기되, 빈 옛
   버킷은 이 수까지 건너뜁니다. 줄이기처럼 빈 버킷이 많아도 옮기기가 빨리
   끝나게 하면서, 한 걸음의 비용을 작게 묶기 위해서입니다. */
#define REHASH_EMPTY_VISITS 10

/* Changes the number of buckets in hash table H to match the
   ideal.  This function can fail because of an out-of-memory
   condition, but that'll just make hash accesses less efficient;
   we can still continue.

   mini-redis perf: 새 버킷 배열을 만들어 설치하고, 옛 배열은
   old_buckets로 넘겨 둡니다. entry는 rehash_step이 이후 호출마다 나눠
   옮깁니다. 원래 코드는 이 자리에서 entry 전부를 한 번에 옮겨서, entry
   약 419만 개에서 서버가 약 330 ms 멈췄습니다(docs/measurements/server/
   003-DA-slower-rehash.md의 e-grow-once). */
static void
rehash (struct hash *h) {
	size_t old_bucket_cnt, new_bucket_cnt;
	struct list *new_buckets;
	size_t i;

	assert (h != NULL);

	/* mini-redis perf: 옮기는 중에는 새 크기 변경을 시작하지 않습니다.
	   배열이 둘을 넘는 상태를 만들지 않기 위해서입니다. */
	if (h->old_buckets != NULL)
		return;

	old_bucket_cnt = h->bucket_cnt;

	/* mini-redis M3: 버킷당 entry 수가 MIN_ELEMS_PER_BUCKET 이상,
	   MAX_ELEMS_PER_BUCKET 이하이면 버킷 수를 바꾸지 않습니다. 위 두 상수의
	   주석이 말하는 동작입니다. 원래 코드는 넣고 지울 때마다 이상적인 버킷 수를
	   다시 계산해서, entry 수가 2의 거듭제곱 경계에 있으면 저장과 삭제가 번갈아
	   올 때마다 entry 전부를 옮겼습니다(docs/milestones/concurrency/spec.md의
	   "측정 결과"). */
	if (h->elem_cnt >= old_bucket_cnt * MIN_ELEMS_PER_BUCKET
	    && h->elem_cnt <= old_bucket_cnt * MAX_ELEMS_PER_BUCKET)
		return;

	/* Calculate the number of buckets to use now.
	   We want one bucket for about every BEST_ELEMS_PER_BUCKET.
	   We must have at least four buckets, and the number of
	   buckets must be a power of 2. */
	new_bucket_cnt = h->elem_cnt / BEST_ELEMS_PER_BUCKET;
	if (new_bucket_cnt < 4)
		new_bucket_cnt = 4;
	while (!is_power_of_2 (new_bucket_cnt))
		new_bucket_cnt = turn_off_least_1bit (new_bucket_cnt);

	/* Don't do anything if the bucket count wouldn't change. */
	if (new_bucket_cnt == old_bucket_cnt)
		return;

	/* Allocate new buckets and initialize them as empty. */
	new_buckets = malloc (sizeof *new_buckets * new_bucket_cnt);
	if (new_buckets == NULL) {
		/* Allocation failed.  This means that use of the hash table will
		   be less efficient.  However, it is still usable, so
		   there's no reason for it to be an error. */
		return;
	}
	for (i = 0; i < new_bucket_cnt; i++)
		list_init (&new_buckets[i]);

	/* Install new bucket info.  mini-redis perf: the old buckets
	   stay in old_buckets until rehash_step has moved them all. */
	h->old_buckets = h->buckets;
	h->old_bucket_cnt = old_bucket_cnt;
	h->rehash_idx = 0;
	h->buckets = new_buckets;
	h->bucket_cnt = new_bucket_cnt;
}

/* mini-redis perf: 옮기는 중이면 비어 있지 않은 옛 버킷 하나를 새 배열로
   옮깁니다. 빈 옛 버킷은 REHASH_EMPTY_VISITS개까지 건너뜁니다. 마지막
   옛 버킷까지 옮기면 옛 배열을 버립니다. */
static void
rehash_step (struct hash *h) {
	size_t empty_visits = 0;

	if (h->old_buckets == NULL)
		return;

	while (h->rehash_idx < h->old_bucket_cnt) {
		struct list *old_bucket = &h->old_buckets[h->rehash_idx++];

		if (!list_empty (old_bucket)) {
			rehash_move_bucket (h, old_bucket);
			break;
		}
		if (++empty_visits >= REHASH_EMPTY_VISITS)
			break;
	}

	if (h->rehash_idx == h->old_bucket_cnt)
		rehash_end (h);
}

/* mini-redis perf: 남은 옛 버킷을 모두 옮기고 옛 배열을 버립니다. */
static void
rehash_finish (struct hash *h) {
	if (h->old_buckets == NULL)
		return;

	while (h->rehash_idx < h->old_bucket_cnt)
		rehash_move_bucket (h, &h->old_buckets[h->rehash_idx++]);
	rehash_end (h);
}

/* Moves each element of OLD_BUCKET into the appropriate bucket of
   H's current bucket array. */
static void
rehash_move_bucket (struct hash *h, struct list *old_bucket) {
	struct list_elem *elem, *next;

	for (elem = list_begin (old_bucket);
			elem != list_end (old_bucket); elem = next) {
		struct list *new_bucket
			= find_bucket (h, list_elem_to_hash_elem (elem));
		next = list_next (elem);
		list_remove (elem);
		list_push_front (new_bucket, elem);
	}
}

/* mini-redis perf: 옛 배열을 버리고 옮기지 않는 상태로 돌아갑니다. 옛
   배열의 버킷은 모두 비었거나, hash_clear가 이미 비웠습니다. */
static void
rehash_end (struct hash *h) {
	free (h->old_buckets);
	h->old_buckets = NULL;
	h->old_bucket_cnt = 0;
	h->rehash_idx = 0;
}

/* Inserts E into BUCKET (in hash table H). */
static void
insert_elem (struct hash *h, struct list *bucket, struct hash_elem *e) {
	h->elem_cnt++;
	list_push_front (bucket, &e->list_elem);
}

/* Removes E from hash table H. */
static void
remove_elem (struct hash *h, struct hash_elem *e) {
	h->elem_cnt--;
	list_remove (&e->list_elem);
}

