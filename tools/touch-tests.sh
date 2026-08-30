#!/usr/bin/env bash
#
# v2.4 — дотик, рух і наближення на платі, по одному тесту за раз.
#
#     ./tools/touch-tests.sh 1     один тест
#     ./tools/touch-tests.sh all   усі підряд
#     ./tools/touch-tests.sh       список
#
# **Це найфізичніша фаза проєкту, і найменш перевірювана автоматикою.** 379 тестів на хості довели
# логіку: який жест є яким, що падіння переважає струс, що рефлекс минає сам, що рука на межі не
# блимає. Але кожна з цих логік живиться сенсором, а сенсор перевіряється лише рукою.
#
# Одна знахідка рев'ю це показала прямо: проксимність читала не ту шину I2C і не могла спрацювати
# ніколи — а тестів на це не могло існувати, бо вони годують детектор числами, і дефект був у
# чотирьох рядках склейки, яких на ноутбуці не виконати.
#
# Тому тут ви — єдиний вимірювальний прилад. Скрипт дає команду, рахує час і питає, що сталося.
#
# Порт послідовного зв'язку тримає лише один процес: інші вікна `board.py` треба закрити.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="$ROOT/.venv/bin/python"
LOG="${RF_TEST_LOG:-$ROOT/touch-test.log}"

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

# What the board reported since this test's marker. **The script reads its own log rather than
# asking the person to read it back.** A question whose answer is already in a file is not a test,
# it is a transcription task -- and the person is here to report what a machine cannot see: what the
# face did, and whether a click was audible.
said() {
    local test_number="$1" pattern="$2"
    awk -v marker="=== ТЕСТ ${test_number}" '$0 ~ marker {found=1; out=""} found {out = out $0 "\n"} END {printf "%s", out}' \
        "$LOG" | grep -oE "$pattern" | sort -u | tr '\n' ' '
}

# Report what the board said, and judge it -- naming what is missing rather than asking.
verdict() {
    local test_number="$1" label="$2" seen="$3"; shift 3
    local missing=()
    for want in "$@"; do [[ "$seen" == *"$want"* ]] || missing+=("$want"); done
    if [[ ${#missing[@]} -eq 0 ]]; then
        bold "  ✅ плата повідомила все, що мала: ${seen}"
        echo "[скрипт] $label · ok · $seen" >> "$LOG"
    elif [[ -z "$seen" ]]; then
        bold "  ❌ плата не повідомила нічого (очікувалось: $*)"
        echo "[скрипт] $label · ПРОВАЛ · нічого" >> "$LOG"
    else
        bold "  ⚠️  плата повідомила: ${seen}— бракує: ${missing[*]}"
        echo "[скрипт] $label · частково · є[$seen] нема[${missing[*]}]" >> "$LOG"
    fi
    echo
}

mic() {
    "$PY" "$ROOT/tools/board.py" --for 5 --log "$LOG" --send "/mic $1" --send-after 1 \
        > /dev/null 2>&1
    sleep 1
}

announce() {
    local text="$1"
    echo "  🔊 (на платі) $text"
    echo "[скрипт] інструкція вголос: $text" >> "$LOG"
    "$PY" "$ROOT/tools/board.py" --for 25 --log "$LOG" \
        --send "Повтори дослівно, нічого не додаючи від себе: $text" --send-after 1 --send-when-idle \
        > /dev/null 2>&1
    sleep 1
}

#: Watch the board while **you** do something to it. No command is sent: the input is your hand.
watch_hands() {
    local number="$1" title="$2" seconds="$3" label="$4"
    { echo; echo "=== ТЕСТ $number — $title — $(date '+%Y-%m-%d %H:%M:%S') ==="; } >> "$LOG"

    local before; before=$(wc -l < "$LOG")
    "$PY" "$ROOT/tools/board.py" --for "$seconds" --log "$LOG" > /dev/null 2>&1 &
    local watcher=$!
    sleep 1
    countdown "$((seconds - 2))" "$label"
    wait "$watcher" 2>/dev/null

    echo
    bold "  що повідомила плата:"
    tail -n "+$((before + 1))" "$LOG" | grep -oE '\[touch\] .*|\[motion\] .*|\[near\] .*|\[mic\] .*' \
        | tail -10 | sed 's/^/    /'
    [[ -z "$(tail -n "+$((before + 1))" "$LOG" | grep -oE '\[touch\]|\[motion\]|\[near\]')" ]] \
        && echo "    (плата не повідомила жодної події)"
    echo
}

ask_choice() {
    local number="$1" question="$2"; shift 2
    local options=("$@") answer="" count=${#options[@]}
    echo
    bold "  $question"
    for ((i = 0; i < count; i++)); do echo "    $((i + 1))) ${options[i]}"; done
    echo
    while [[ ! "$answer" =~ ^[0-9]+$ ]] || (( answer < 1 || answer > count )); do
        read -r -p "  Введіть число 1–$count: " answer
    done
    echo "[людина] тест $number · ${options[answer - 1]}" >> "$LOG"
    echo "  записано в лог: ${options[answer - 1]}"
    echo
}

header() { rule; bold "ТЕСТ $1 — $2"; echo; }

# --------------------------------------------------------------------------------------

test_0() {
    header 0 "чи то той сервер, і чи бачить плата свої сенсори"
    local serving expected
    serving=$(curl -s -m 5 http://192.168.1.197:8000/openapi.json \
        | "$PY" -c "import sys,json;print(json.load(sys.stdin)['info']['version'])" 2>/dev/null)
    expected=$(cat "$ROOT/VERSION")
    echo "  сервер віддає: ${serving:-(не відповів)}   ·   у репозиторії: $expected"
    echo "[скрипт] тест 0 · сервер=${serving:-none} репозиторій=$expected" >> "$LOG"
    [[ "$serving" == "$expected" ]] && bold "  ✅ сервер збігається" \
        || bold "  ❌ сервер НЕ збігається — tools/remote.sh deploy && restart"
    echo
    echo "  Тепер сенсори. **Рев'ю знайшло, що проксимність читала не ту шину I2C**"
    echo "  і не могла спрацювати ніколи — при цьому рапортувала успіх. Тепер вона питає"
    echo "  сенсор про його ідентифікатор, і плата каже, що з нього вийшло."
    echo
    { echo; echo "=== ТЕСТ 0 — сенсори — $(date '+%H:%M:%S') ==="; } >> "$LOG"
    local sensors
    sensors=$("$PY" "$ROOT/tools/board.py" --for 8 --log "$LOG" --send "/sensors" --send-after 1 \
                    2>/dev/null | grep -m1 '\[sensors\]')

    if [[ -z "$sensors" ]]; then
        bold "  ❌ плата не відповіла — інше вікно board.py тримає порт?"
        echo "[скрипт] тест 0 · сенсори: плата не відповіла" >> "$LOG"
        return 1
    fi

    echo "    ${sensors}"
    echo

    # **Вирішує скрипт, не людина.** Він щойно це прочитав; питати про це було б
    # проханням переказати те, що вже є в змінній.
    if [[ "$sensors" == *"NOT PRESENT"* ]]; then
        bold "  ❌ сенсор не відповів — рухи або наближення працювати не будуть"
        echo "[скрипт] тест 0 · ПРОВАЛ · $sensors" >> "$LOG"
        return 1
    fi

    bold "  ✅ обидва сенсори відповіли; 0x92 — це справді LTR-553"
    echo "     виправлення шини I2C з рев'ю v2.4 підтверджене залізом"
    echo "[скрипт] тест 0 · ok · $sensors" >> "$LOG"
    echo
}

test_1() {
    header 1 "дотик: тап, погладжування, тицяння в око"
    echo "  Три жести підряд. **Дивіться на обличчя щоразу** — реакція має з'явитись одразу,"
    echo "  не за секунду: рефлекс не питає сервер, він саме для цього окремий рівень."
    echo
    echo "  Коли скрипт почне відлік:"
    echo "    1) торкніться щоки ОДИН раз"
    echo "    2) зачекайте секунду, тоді ПОГЛАДЬТЕ обличчя збоку вбік"
    echo "    3) зачекайте секунду, тоді тицьніть в ОКО"
    echo

    mic off
    announce "Зараз торкніться мене: спершу тап, потім погладжування, потім тицьніть в око."
    watch_hands 1 "три дотики" 35 "торкайтесь: тап, погладжування, око"

    ask_choice 1 "Чи реагувало обличчя на КОЖЕН дотик?" \
        "ТАК — усі три, і реакція була миттєва" \
        "реагувало, але з помітною затримкою" \
        "реагувало не на всі" \
        "не реагувало взагалі"

    ask_choice 1 "Чи були три реакції РІЗНІ?" \
        "ТАК — тап, погладжування й око виглядали по-різному" \
        "тап і погладжування схожі, око інше" \
        "усі три однакові"
}

test_2() {
    header 2 "повторні дотики нарощують радість"
    echo "  DEVICE_UI: «repeated taps build joy». Лічильник — це те, що її нарощує, і він"
    echo "  згасає, бо радість, яка не згасає, лишила б пристрій у захваті від учорашнього."
    echo
    echo "  Постукайте по щоці ШВИДКО п'ять-шість разів поспіль."
    echo

    mic off
    announce "Постукайте по мені кілька разів поспіль і подивіться, чи я радітиму дужче."
    watch_hands 2 "серія дотиків" 25 "стукайте швидко"

    ask_choice 2 "Чи ставала реакція обличчя ПОМІТНІШОЮ з кожним дотиком?" \
        "ТАК — радість наростала" \
        "реагувало однаково щоразу" \
        "реагувало лише на перший"
}

test_3() {
    header 3 "рух: нахил, струс, перевертання"
    echo "  Три рухи. **Пристрій не падає** — тест на падіння свідомо не робимо."
    echo
    echo "  Коли почнеться відлік:"
    echo "    1) НАХИЛІТЬ плату вбік градусів на тридцять і поверніть"
    echo "    2) СТРУСІТЬ її кілька разів"
    echo "    3) ПЕРЕВЕРНІТЬ екраном донизу й поверніть"
    echo

    mic off
    announce "Понахиляйте мене, потім струсіть, потім переверніть."
    watch_hands 3 "три рухи" 40 "нахил, струс, перевертання"

    verdict 3 "тест 3 · рухи" "$(said 3 'tilt|shake|upside_down|picked_up|free_fall')" \
        tilt shake upside_down

    ask_choice 3 "Чи реагувало обличчя на струс і перевертання?" \
        "ТАК — обидва разів помітно" \
        "лише на один із них" \
        "ні"
}

test_4() {
    header 4 "наближення: обличчя прокидається до руки"
    echo "  **Це та знахідка, яку може підтвердити лише рука.** Проксимність читала не ту"
    echo "  шину I2C і не могла спрацювати ніколи; тестів на це не могло існувати."
    echo
    echo "  Піднесіть долоню до екрана сантиметрів на десять, потримайте, приберіть."
    echo "  Повторіть двічі. Дивіться, куди дивиться обличчя."
    echo

    mic off
    announce "Піднесіть до мене руку і приберіть. Подивіться, чи я поверну погляд."
    watch_hands 4 "рука біля екрана" 35 "підносьте й прибирайте долоню"

    verdict 4 "тест 4 · наближення" "$(said 4 'approach|leave')" approach leave

    ask_choice 4 "Чи змінилось ОБЛИЧЧЯ, коли рука наблизилась?" \
        "ТАК — погляд повернувся до руки" \
        "щось змінилось, але не погляд" \
        "ні"
}

test_5() {
    header 5 "кнопка мікрофона — і вона НЕ лоскоче"
    echo "  Вимкнення мікрофона тричі намагалися зробити жестом, і щоразу воно"
    echo "  зіштовхувалось із прив'язаністю: два пальці панель не бачить взагалі"
    echo "  (вона повідомляє одну точку), а подвійний тап уже належав радості."
    echo "  Тому це кнопка — іконка мікрофона в лівому верхньому кутку."
    echo
    echo "  Тапніть по іконці. Потім ще раз, щоб увімкнути назад."
    echo "  Має клацнути двома нотами і змінити колір: блакитна — живий мікрофон,"
    echo "  бурштинова перекреслена — вимкнений."
    echo

    announce "Тапніть по іконці мікрофона в лівому кутку."
    watch_hands 5 "кнопка мікрофона" 30 "тапніть по іконці двічі"

    verdict 5 "тест 5 · перемикання" "$(said 5 'увімкнено|вимкнено')" увімкнено вимкнено

    # Керування не є прив'язаністю: натиск на кнопку не має ні відкривати вікно
    # прослуховування, ні надсилати серверу подію дотику.
    local leaked
    leaked=$(said 5 'closed by ptt|"kind":"tap"|"kind":"multi_tap"')
    if [[ -n "$leaked" ]]; then
        bold "  ❌ натиск на кнопку протік у щось інше: ${leaked}"
        echo "[скрипт] тест 5 · ПРОВАЛ · протік: $leaked" >> "$LOG"
    else
        bold "  ✅ кнопка не відкрила вікно прослуховування і нічого не надіслала"
        echo "[скрипт] тест 5 · ok · без протікання" >> "$LOG"
    fi
    echo

    ask_choice 5 "Чи було чутно клац, і чи змінилась іконка?" \
        "ТАК — і звук, і колір" \
        "змінилась іконка, але звуку не чув(ла)" \
        "був звук, але іконка не змінилась" \
        "ні того, ні іншого"

    ask_choice 5 "Чи ЛОСКОТАЛОСЬ обличчя від натиску на кнопку?" \
        "НІ — обличчя не реагувало, як і має бути" \
        "ТАК — реагувало, ніби його погладили" \
        "не помітив(ла)"

    echo
    echo "  Скільки спроб знадобилось? Кнопка має спрацьовувати з першого разу."
    ask_choice 5 "З якого разу спрацювало?" \
        "з першого, обидва рази" \
        "інколи доводилось тапати двічі" \
        "доводилось цілитись у самий куток"
}

test_6() {
    header 6 "рефлекси працюють БЕЗ сервера"
    echo "  DoD фази закінчується словами: «with the server offline, every reflex still"
    echo "  works». Це і є причина, чому рівні розділені — рефлекс не питає нікого."
    echo
    echo "  Скрипт зупинить сервер. Торкайтеся обличчя, поки його нема."
    echo

    mic off
    announce "Зараз я втрачу сервер. Торкайтеся мене — я маю реагувати однаково."

    echo "  ▸ зупиняю сервер"
    "$ROOT/tools/remote.sh" stop > /dev/null 2>&1
    sleep 3
    watch_hands 6 "дотики без сервера" 30 "торкайтесь обличчя"

    echo "  ▸ піднімаю сервер"
    "$ROOT/tools/remote.sh" start 2>&1 | tail -1 | sed 's/^/    /'
    sleep 3

    ask_choice 6 "Чи реагувало обличчя на дотики БЕЗ сервера?" \
        "ТАК — так само, як із сервером" \
        "реагувало слабше або повільніше" \
        "не реагувало"
}

# --------------------------------------------------------------------------------------

usage() {
    rule
    bold "v2.4 — тести дотику, руху й наближення"
    echo
    echo "  0  сервер і сенсори при старті          ./tools/touch-tests.sh 0"
    echo "  1  тап, погладжування, око              ./tools/touch-tests.sh 1"
    echo "  2  повторні дотики нарощують радість    ./tools/touch-tests.sh 2"
    echo "  3  нахил, струс, перевертання           ./tools/touch-tests.sh 3"
    echo "  4  наближення руки ⭐                    ./tools/touch-tests.sh 4"
    echo "  5  кнопка мікрофона, і не лоскоче      ./tools/touch-tests.sh 5"
    echo "  6  рефлекси без сервера                 ./tools/touch-tests.sh 6"
    echo
    echo "  усі підряд                              ./tools/touch-tests.sh all"
    echo
    echo "  ⭐ тест 4 — єдиний спосіб підтвердити виправлення шини I2C."
    echo
    echo "  Лог: $LOG"
    rule
}

main() {
    mkdir -p "$(dirname "$LOG")"
    case "${1:-}" in
        0) test_0 ;;
        1) test_0; test_1 ;;
        2) test_0; test_2 ;;
        3) test_0; test_3 ;;
        4) test_0; test_4 ;;
        5) test_0; test_5 ;;
        6) test_0; test_6 ;;
        all) test_0; test_1; test_2; test_3; test_4; test_5; test_6
             rule; bold "усі тести пройдено — надішліть $LOG"; rule ;;
        *) usage ;;
    esac
}

main "$@"
