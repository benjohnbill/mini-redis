"""상품 id에서 실제로 있을 법한 상품 이름을 만듭니다.

이름은 "브랜드 품목 색상 세대"입니다. 네 목록의 조합은 COMBINATIONS개이고,
id에 COMBINATIONS와 서로소인 수를 곱해 조합을 고르므로 이웃한 id는 다른 이름을
받고, 같은 이름은 COMBINATIONS개 간격으로 다시 나옵니다. 상품 100만 개에서
전체 이름 하나는 약 17번 나오므로, 그 이름을 검색하면 원본은 이름을 끝까지
읽게 됩니다. "무선 이어폰"처럼 일부만 검색하면 금방 20개를 찾습니다.
"""

BRANDS = ["한빛", "누리", "다온", "모아", "새솔", "온결", "가람", "하랑",
          "아라", "미르"]
CATEGORIES = [
    "무선 이어폰", "블루투스 스피커", "캠핑 의자", "전기 포트", "보조 배터리",
    "기계식 키보드", "무선 마우스", "가습기", "공기청정기", "커피 그라인더",
    "에어프라이어", "스마트 워치", "태블릿 거치대", "노트북 파우치", "LED 스탠드",
    "전동 칫솔", "헤어 드라이어", "미니 선풍기", "등산 배낭", "요가 매트",
    "텀블러", "멀티탭", "웹캠", "게이밍 헤드셋", "휴대용 모니터",
    "무선 충전기", "USB 허브", "캠핑 랜턴", "손난로", "체중계",
]
COLORS = ["블랙", "화이트", "그레이", "네이비", "베이지", "카키", "실버",
          "로즈골드", "민트", "버건디"]
VARIANTS = [f"{generation}세대" for generation in range(1, 21)]

COMBINATIONS = len(BRANDS) * len(CATEGORIES) * len(COLORS) * len(VARIANTS)
# COMBINATIONS(60,000 = 2^5 * 3 * 5^4)와 서로소라서 1..COMBINATIONS의 id가
# 조합 하나씩을 겹치지 않게 받습니다.
SPREAD = 7919


def product_name(product_id):
    index = product_id * SPREAD % COMBINATIONS
    index, variant = divmod(index, len(VARIANTS))
    index, color = divmod(index, len(COLORS))
    brand, category = divmod(index, len(CATEGORIES))

    return " ".join((BRANDS[brand], CATEGORIES[category], COLORS[color],
                     VARIANTS[variant]))
