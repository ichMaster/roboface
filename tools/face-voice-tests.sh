#!/usr/bin/env bash
#
# v2.5 + v2.6 — голос, погляд і п'ять облич, по одному тесту за раз.
#
#     ./tools/face-voice-tests.sh 1     один тест
#     ./tools/face-voice-tests.sh all   усі підряд
#     ./tools/face-voice-tests.sh       список
#
# **Правило цього файлу, здобуте дорогою ціною в v2.4:** скрипт судить про те, що сказала плата;
# людина — про те, що зробило обличчя. Питання, на яке скрипт має відповідь у власному лозі, він
# не ставить. У v2.4 тест 0 просив переказати рядки, які сам щойно записав, і це було не тестом,
# а вправою з переписування.
#
# Друге правило, звідти ж: **число має пережити момент вимірювання.** Людина з рукою біля плати і
# людина біля терміналу — не одна людина, тож плата запам'ятовує піки (`/mic-levels`, `/touch`,
# `/sensors`), а скрипт зчитує їх потім.
#
# Порт послідовного зв'язку тримає лише один процес: інші вікна `board.py` треба закрити.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="$ROOT/.venv/bin/python"
LOG="${RF_TEST_LOG:-$ROOT/face-voice-test.log}"
SERVER="${RF_SERVER:-192.168.1.197:8000}"

bold() { printf '\033[1m%s\033[0m\n' "$*"; }
rule() { printf '────────────────────────────────────────────────────────\n'; }

header() {
    echo; rule; bold "ТЕСТ $1 — $2"; echo
    { echo; echo "=== ТЕСТ $1 — $2 — $(date '+%Y-%m-%d %H:%M:%S') ==="; } >> "$LOG"
}

countdown() {
    local seconds="$1" label="$2"
    for ((i = seconds; i > 0; i--)); do printf "\r  ▶ %s: %2d с  " "$label" "$i"; sleep 1; done
    printf "\r  ▶ %s: готово          \n" "$label"
}

# Одна команда платі, один рядок назад. Маркер `ЗАПИТ`, не `ТЕСТ` — інакше він зсунув би вікно
# пошуку `said` за все, що тест щойно записав (саме цей баг завалив тест 4 у v2.4, коли той
# насправді пройшов).
board_line() {
    local n="$1" command="$2" pattern="$3"
    { echo; echo "=== ЗАПИТ $n — $command — $(date '+%H:%M:%S') ==="; } >> "$LOG"
    "$PY" "$ROOT/tools/board.py" --for 7 --log "$LOG" --send "$command" --send-after 1 2>/dev/null \
        | grep -oE "$pattern" | tail -1
}

watch_hands() {
    local n="$1" label="$2" seconds="$3" instruction="$4"
    echo "  ▸ $instruction"
    "$PY" "$ROOT/tools/board.py" --for "$seconds" --log "$LOG" > /dev/null 2>&1 &
    local watcher=$!
    countdown "$((seconds - 2))" "$label"
    wait "$watcher" 2>/dev/null
}

said() {
    local n="$1" pattern="$2"
    awk -v marker="=== ТЕСТ ${n}" '$0 ~ marker {found=1; out=""} found {out = out $0 "\n"} END {printf "%s", out}' \
        "$LOG" | grep -oE "$pattern" | sort -u | tr '\n' ' '
}

ask_choice() {
    local n="$1" question="$2"; shift 2
    echo; bold "  $question"
    local i=1
    for option in "$@"; do echo "    $i) $option"; i=$((i + 1)); done
    echo
    local answer
    read -r -p "  Введіть число 1–$#: " answer
    local chosen="${!answer:-(немає відповіді)}"
    [[ "$answer" =~ ^[0-9]+$ ]] && chosen="$(eval echo "\${$((answer + 2))}")" 2>/dev/null || true
    echo "[відповідь] тест $n · $question → $answer" >> "$LOG"
}

# ---------------------------------------------------------------------------------------

test_0() {
    header 0 "сервер, сенсори, мікрофони й п'ять облич"

    local serving expected
    serving=$(curl -s -m 5 "http://$SERVER/openapi.json" \
        | "$PY" -c "import sys,json;print(json.load(sys.stdin)['info']['version'])" 2>/dev/null)
    expected=$(cat "$ROOT/VERSION")
    echo "  сервер віддає: ${serving:-(не відповів)}   ·   у репозиторії: $expected"
    if [[ "$serving" == "$expected" ]]; then bold "  ✅ сервер збігається"
    else bold "  ❌ сервер НЕ збігається — tools/remote.sh deploy && restart"; fi
    echo

    local sensors
    sensors=$(board_line 0 "/sensors" '\[sensors\].*')
    echo "  ${sensors:-(плата не відповіла)}"
    [[ "$sensors" == *"NOT PRESENT"* ]] && bold "  ❌ сенсор не відповів" || bold "  ✅ сенсори на місці"
    echo

    local levels
    levels=$(board_line 0 "/mic-levels" 'left=[0-9.]+ right=[0-9.]+')
    echo "  ${levels:-(немає рівнів)}"
    if [[ -n "$levels" ]]; then bold "  ✅ обидва канали читаються"
    else bold "  ❌ мікрофони мовчать"; fi
    echo

    # П'ять облич: плата рапортує кожне при старті, тож досить спитати.
    local skins
    skins=$("$PY" "$ROOT/tools/board.py" --for 7 --log "$LOG" --send "/skins" --send-after 1 2>/dev/null \
            | grep -cE '^\[skins\]   ')
    echo "  облич у прошивці: ${skins:-0}"
    if [[ "${skins:-0}" == "5" ]]; then bold "  ✅ усі п'ять на місці"
    else bold "  ⚠️  очікувалось 5"; fi
    echo
}

test_1() {
    header 1 "погляд повертається до голосу"
    echo "  v2.5. Напрямок береться з різниці рівнів між двома мікрофонами; часову різницю"
    echo "  тут виміряти неможливо — 40 мм рознесення це ±2 семпли на 16 кГц."
    echo
    echo "  Під час відліку: скажіть кілька фраз, стоячи ЛІВОРУЧ від плати, потім ПРАВОРУЧ."
    echo "  Дивіться на очі — вони мають повертатись у ваш бік."
    echo

    board_line 1 "/mic-levels" 'нічого' > /dev/null   # скидає діапазон

    watch_hands 1 "говоріть з боків" 35 "ліворуч, потім праворуч"

    local range
    range=$(board_line 1 "/mic-levels" 'діапазон від старту: [-+0-9.]+ \.\. [-+0-9.]+')
    echo "  ${range:-(немає діапазону)}"
    local direction
    direction=$(board_line 1 "/direction" '\[direction\].*')
    echo "  ${direction:-(немає напрямку)}"
    echo

    # Обидва знаки = два мікрофони, які справді чують по-різному. Це скрипт бачить сам.
    if [[ "$range" == *-* && "$range" == *+* ]]; then
        bold "  ✅ баланс хитнувся в обидва боки — напрямок вимірюваний"
    else
        bold "  ⚠️  баланс не пішов в обидва боки; спробуйте стати ближче до країв"
    fi

    ask_choice 1 "Чи повертався ПОГЛЯД у ваш бік?" \
        "ТАК — і ліворуч, і праворуч" \
        "лише в один бік" \
        "щось рухалось, але не туди" \
        "ні"
}

test_2() {
    header 2 "перекритий мікрофон не псує звук"
    echo "  v2.5, RF-076. На сервер іде середнє двох каналів — це не може клікнути і вдвічі"
    echo "  зменшує некорельований шум. Якщо один мікрофон накрити, плата має перейти на"
    echo "  другий, а не домішувати тишу."
    echo
    echo "  Під час відліку: накрийте пальцем ОДИН мікрофон і поговоріть; потім заберіть."
    echo

    watch_hands 2 "накрийте один мікрофон" 25 "накрити, поговорити, забрати"

    local source
    source=$(board_line 2 "/mic-levels" 'на сервер іде: .*')
    echo "  ${source:-(немає джерела)}"
    echo

    ask_choice 2 "Чи змінилось джерело, поки мікрофон був накритий?" \
        "не перевіряв(ла) під час накривання" \
        "так — показувало «лише лівий» або «лише правий»" \
        "ні — весь час «середнє двох»"

    echo "  (Друге — правильно. Третє теж прийнятне: поріг навмисне високий, щоб голос збоку"
    echo "   не сплутати з несправністю.)"
}

test_3() {
    header 3 "п'ять облич"
    echo "  v2.6. Одна граматика виразу, п'ять силуетів. Скрипт перемкне кожне на 6 секунд."
    echo

    for name in stackchan ghost flame jelly cloud; do
        "$PY" "$ROOT/tools/board.py" --for 5 --log "$LOG" --send "/skin $name" --send-after 1 \
            > /dev/null 2>&1
        bold "  → $name"
        sleep 4
    done
    echo

    ask_choice 3 "Скільки облич відмалювалось РІЗНО і впізнавано?" \
        "усі п'ять" \
        "чотири" \
        "три або менше" \
        "деякі були порожні або зламані"

    ask_choice 3 "Чи лишались очі й рот на місці на кожному з них?" \
        "так, скрізь у межах обличчя" \
        "десь виїжджали за смуги" \
        "не помітив(ла)"
}

test_4() {
    header 4 "емоції на духах"
    echo "  DoD фази: усі п'ять облич відмальовують ОДИН і той самий EmotionFrame."
    echo "  У полум'я і хмари колір тіла — це і є настрій, тож різниця має бути очевидна."
    echo

    "$PY" "$ROOT/tools/board.py" --for 5 --log "$LOG" --send "/skin flame" --send-after 1 > /dev/null 2>&1
    bold "  → полум'я"
    for face in joy sad error; do
        "$PY" "$ROOT/tools/board.py" --for 5 --log "$LOG" --send "/face $face" --send-after 1 \
            > /dev/null 2>&1
        echo "    $face"
        sleep 3
    done
    echo

    ask_choice 4 "Чи змінювало полум'я КОЛІР під настрій?" \
        "так — радість тепла, сум синій, помилка червона" \
        "змінювалось обличчя, але не колір" \
        "нічого не змінювалось"
}

test_5() {
    header 5 "карусель"
    echo "  v2.6, RF-082. Затримайте палець на обличчі 1.2 с БЕЗ мови — відкриється смужка"
    echo "  крапок унизу. Ведіть палець, не відпускаючи: обличчя мінятиметься одразу."
    echo "  Відпустіть на крапці — вибір; відпустіть ВИЩЕ смужки — скасування."
    echo
    echo "  Це єдиний жест, який щось відбирає: утримання вже відкрило мікрофон, і через"
    echo "  1.2 с плата вирішує, що ви мали на увазі інше. Тому скасувати має бути легко."
    echo

    watch_hands 5 "спробуйте карусель" 40 "затримати, повести, відпустити — і ще раз зі скасуванням"

    # Керування не є прив'язаністю: карусель не має ні лоскотати, ні слати подію дотику.
    local leaked
    leaked=$(said 5 'stroke|"kind":"tap"|multi_tap')
    if [[ -n "$leaked" ]]; then
        bold "  ❌ карусель протікла в прив'язаність: ${leaked}"
    else
        bold "  ✅ жодного фантомного дотику після каруселі"
    fi
    echo

    ask_choice 5 "Чи відкрилась смужка крапок?" \
        "так, і обличчя мінялось під пальцем" \
        "відкрилась, але обличчя не мінялось" \
        "не відкрилась"

    ask_choice 5 "Чи спрацювало СКАСУВАННЯ (відпустити вище смужки)?" \
        "так — повернулось попереднє обличчя" \
        "ні — вибрало те, що було під пальцем" \
        "не пробував(ла)"

    ask_choice 5 "Чи ЛИШИЛАСЬ смужка, поки палець на екрані?" \
        "так" \
        "ні — зникла сама, поки я тримав(ла)"

    echo "  (Останнє питання — про виправлення #2: відкриття каруселі закриває вікно"
    echo "   прослуховування, сервер відповідає на уривок, і ця відповідь колись забирала"
    echo "   смужку з-під пальця.)"
}

test_6() {
    header 6 "сервер перемикає обличчя"
    echo "  v2.6, RF-081 + виправлення #4. Кадр config_updated існував, і його ніхто не слав —"
    echo "  DoD «перемикається в обидва боки» був наполовину неправдою. Тепер є ендпоінт."
    echo

    for name in ghost cloud stackchan; do
        local answer
        answer=$(curl -s -m 5 -X POST "http://$SERVER/face/$name")
        echo "    POST /face/$name → $answer"
        echo "[скрипт] тест 6 · $name → $answer" >> "$LOG"
        if [[ "$answer" != *'"devices":1'* ]]; then
            bold "  ⚠️  плата не була підключена на цьому кроці"
        fi
        sleep 4
    done
    echo

    local bad
    bad=$(curl -s -m 5 -o /dev/null -w '%{http_code}' -X POST "http://$SERVER/face/dragon")
    if [[ "$bad" == "404" ]]; then
        bold "  ✅ невідоме обличчя — 404, а не тиха згода"
    else
        bold "  ❌ невідоме обличчя повернуло $bad"
    fi
    echo

    ask_choice 6 "Чи мінялось обличчя на екрані від команд сервера?" \
        "так, усі три" \
        "деякі" \
        "ні"
}

# ---------------------------------------------------------------------------------------

case "${1:-}" in
    0) test_0 ;; 1) test_1 ;; 2) test_2 ;; 3) test_3 ;; 4) test_4 ;; 5) test_5 ;; 6) test_6 ;;
    all) for n in 0 1 2 3 4 5 6; do "test_$n"; done ;;
    *)
        rule
        bold "v2.5 + v2.6 — голос, погляд і п'ять облич"
        echo
        echo "  0  сервер, сенсори, мікрофони, обличчя   ./tools/face-voice-tests.sh 0"
        echo "  1  погляд повертається до голосу ⭐       ./tools/face-voice-tests.sh 1"
        echo "  2  перекритий мікрофон                   ./tools/face-voice-tests.sh 2"
        echo "  3  п'ять облич                           ./tools/face-voice-tests.sh 3"
        echo "  4  емоції на духах                       ./tools/face-voice-tests.sh 4"
        echo "  5  карусель ⭐                            ./tools/face-voice-tests.sh 5"
        echo "  6  сервер перемикає обличчя              ./tools/face-voice-tests.sh 6"
        echo
        echo "  усі підряд                               ./tools/face-voice-tests.sh all"
        echo
        echo "  ⭐ 1 і 5 — те, що не має жодного тесту на хості й не може мати."
        echo
        echo "  Лог: $LOG"
        rule
        ;;
esac
