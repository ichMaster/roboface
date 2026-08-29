#!/usr/bin/env bash
#
# v1.4 — тести на платі, по одному за раз.
#
#     ./tools/hw-tests.sh 1     один тест
#     ./tools/hw-tests.sh all   усі шість підряд
#     ./tools/hw-tests.sh       список тестів
#
# Кожен тест запускається окремо, щоб ви були біля пристрою тоді, коли самі захочете. Скрипт сам
# відлічує час і сам подає команди платі; від вас потрібно тільки мовчати або говорити, коли він
# просить. Усе, що надрукувала плата, дописується в один лог — надсилати треба саме його, а не
# переписані з екрана числа.
#
# Порт послідовного зв'язку одночасно тримає лише один процес, тому інші вікна `board.py` перед
# запуском треба закрити — інакше плата не відповість, і це виглядатиме як несправність.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="$ROOT/.venv/bin/python"
LOG="${RF_TEST_LOG:-$ROOT/hw-test.log}"

bold() { printf '\033[1m%s\033[0m\n' "$*"; }
rule() { printf '────────────────────────────────────────────────────────\n'; }

countdown() {
    local seconds="$1" label="$2"
    for ((i = seconds; i > 0; i--)); do
        printf "\r  ▶ %s: %2d с  " "$label" "$i"
        sleep 1
    done
    printf "\r  ▶ %s: готово          \n" "$label"
}

# Заголовок у лог, board.py у фоні, відлік для людини, потім витяг рядків саме цього тесту.
watch_board() {
    local number="$1" title="$2" seconds="$3" command="$4" label="$5"
    {
        echo
        echo "=== ТЕСТ $number — $title — $(date '+%Y-%m-%d %H:%M:%S') ==="
    } >> "$LOG"

    local args=(--for "$seconds" --log "$LOG")
    [[ -n "$command" ]] && args+=(--send "$command" --send-after 2)

    local before
    before=$(wc -l < "$LOG")

    "$PY" "$ROOT/tools/board.py" "${args[@]}" > /dev/null 2>&1 &
    local watcher=$!
    sleep 2
    countdown "$((seconds - 3))" "$label"
    wait "$watcher" 2>/dev/null

    echo
    bold "  що сказала плата:"
    tail -n "+$((before + 1))" "$LOG" \
        | grep -E '\[cal\]|\[heard\]|\[listen\]|\[state\]|\[vad\]|\[mic\]' \
        | tail -8 | sed 's/^/    /'
    [[ ${PIPESTATUS[1]} -ne 0 ]] && echo "    (плата нічого не сказала)"
    echo
}

# Те єдине, чого немає в лозі: чи пролунала відповідь. Мікрофон плати цього не запише, а людина
# біля неї знає напевно -- тож питаємо і кладемо відповідь у той самий лог, щоб її не довелось
# переказувати окремим повідомленням і щоб вона не загубилась між тестами.
ask_heard() {
    local number="$1" answer=""
    echo
    bold "  Чи почули ви відповідь УГОЛОС із динаміка плати?"
    echo "    1) так, чув(ла)"
    echo "    2) ні, тиша"
    echo "    3) щось було, але нерозбірливо / обірвано"
    echo
    while [[ ! "$answer" =~ ^[123]$ ]]; do
        read -r -p "  Введіть 1, 2 або 3: " answer
    done
    local text
    case "$answer" in
        1) text="ТАК — відповідь було чути" ;;
        2) text="НІ — тиша" ;;
        3) text="ЧАСТКОВО — звук був, але нерозбірливий або обірваний" ;;
    esac
    echo "[людина] тест $number · звук: $text" >> "$LOG"
    echo "  записано в лог: $text"
    echo
}

header() {
    rule
    bold "ТЕСТ $1 — $2"
    echo
}

footer() {
    rule
    echo "  Лог: $LOG"
    echo "  $1"
    echo
}

test_1() {
    header 1 "рівень кімнати"
    echo "  ЩО РОБИТИ:  нічого. Просто тиша в кімнаті, як зазвичай."
    echo "  ЧОГО ЧЕКАТИ: рядок [cal] з чотирма відсотками — це шум, який VAD"
    echo "               НЕ має приймати за мову."
    echo
    watch_board 1 "рівень кімнати" 15 "/cal 10" "МОВЧІТЬ"
    footer "Далі: ./tools/hw-tests.sh 2"
}

test_2() {
    header 2 "звичайний голос"
    echo "  ЩО РОБИТИ:  говоріть безперервно, звичайним голосом, зі своєї"
    echo "              звичайної відстані. НЕ голосніше, ніж завжди."
    echo "  ЧОГО ЧЕКАТИ: p90 помітно вище, ніж у тесті 1. Якщо однакове —"
    echo "               мікрофон не чує голос, і поріг тут не допоможе."
    echo
    watch_board 2 "звичайний голос" 15 "/cal 10" "ГОВОРІТЬ звичайно"
    footer "Далі: ./tools/hw-tests.sh 3"
}

test_3() {
    header 3 "тихий голос або здалеку"
    echo "  ЩО РОБИТИ:  те саме, але тихіше чи відсунувшись — найгірший"
    echo "              випадок, який ще має працювати."
    echo "  ЧОГО ЧЕКАТИ: p90 усе ще вище, ніж у тесті 1. Це нижня межа,"
    echo "               під яку поріг опускати не можна."
    echo
    watch_board 3 "тихий голос" 15 "/cal 10" "ГОВОРІТЬ тихіше"
    footer "Надішліть лог після тестів 1–3 — я порахую поріг. Далі: 4"
}

test_4() {
    header 4 "hands-free оберт"
    echo "  ЩО РОБИТИ:  НІЧОГО не натискайте. Скажіть одну фразу українською"
    echo "              і замовкніть. Слухайте, чи плата відповість УГОЛОС."
    echo "  ЧОГО ЧЕКАТИ: [state] listening → [heard] ваша фраза →"
    echo "               [state] thinking → replying → idle, і голос із динаміка."
    echo
    watch_board 4 "hands-free оберт" 30 "" "ГОВОРІТЬ фразу, потім тиша"
    ask_heard 4
    footer "Далі: ./tools/hw-tests.sh 5"
}

test_5() {
    header 5 "хибні спрацювання"
    echo "  ЩО РОБИТИ:  нічого. 30 секунд тиші."
    echo "  ЧОГО ЧЕКАТИ: жодного [state] listening. Якщо вікно відкривається"
    echo "               саме — поріг замалий і його треба піднімати."
    echo
    watch_board 5 "хибні спрацювання" 35 "" "ТИША"
    footer "Далі: ./tools/hw-tests.sh 6"
}

test_6() {
    header 6 "утримання екрана — запасний шлях"
    echo "  ЩО РОБИТИ:  притисніть і тримайте екран ~3 с, скажіть фразу,"
    echo "              відпустіть. Слухайте, чи буде відповідь уголос."
    echo "  ЧОГО ЧЕКАТИ: [listen] ... sent N frames, де N ≈ 50 на кожну"
    echo "               секунду утримання (тобто ~150 за 3 с)."
    echo
    watch_board 6 "утримання екрана" 25 "" "ТРИМАЙТЕ і говоріть"
    ask_heard 6
    footer "Готово — у лозі вже все, зокрема ваші відповіді про звук."
}

usage() {
    rule
    bold "RoboFace v1.4 — тести на платі"
    echo
    echo "  ./tools/hw-tests.sh 1    рівень кімнати ......... мовчати 10 с"
    echo "  ./tools/hw-tests.sh 2    звичайний голос ........ говорити 10 с"
    echo "  ./tools/hw-tests.sh 3    тихий голос ............ говорити тихо 10 с"
    echo "  ./tools/hw-tests.sh 4    hands-free оберт ....... фраза, потім тиша"
    echo "  ./tools/hw-tests.sh 5    хибні спрацювання ...... 30 с тиші"
    echo "  ./tools/hw-tests.sh 6    утримання екрана ....... тримати і говорити"
    echo
    echo "  ./tools/hw-tests.sh all  усі шість підряд"
    echo
    echo "  Лог дописується в: $LOG"
    echo "  Перед запуском закрийте інші вікна board.py."
    rule
}

case "${1:-}" in
    1) test_1 ;;
    2) test_2 ;;
    3) test_3 ;;
    4) test_4 ;;
    5) test_5 ;;
    6) test_6 ;;
    all) test_1; test_2; test_3; test_4; test_5; test_6 ;;
    *) usage ;;
esac
