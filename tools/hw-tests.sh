#!/usr/bin/env bash
#
# v1.4 — шість тестів на платі, які запускаються самостійно.
#
# Скрипт сам відлічує час і сам подає команди платі; від вас потрібно тільки мовчати або говорити,
# коли він про це просить. Усе, що надрукувала плата, потрапляє в один лог — саме його треба буде
# надіслати, а не переписувати числа з екрана.
#
#     ./tools/hw-tests.sh
#
# Порт послідовного зв'язку одночасно тримає лише один процес, тому інші вікна `board.py` перед
# запуском треба закрити — інакше плата просто не відповість, і це виглядатиме як несправність.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="$ROOT/.venv/bin/python"
LOG="${RF_TEST_LOG:-$ROOT/hw-test.log}"

bold() { printf '\033[1m%s\033[0m\n' "$*"; }
rule() { printf '%s\n' "────────────────────────────────────────────────────────"; }

# Відлік уголос, щоб не треба було дивитись на годинник.
countdown() {
    local seconds="$1" label="$2"
    for ((i = seconds; i > 0; i--)); do
        printf "\r  %s: %2d с  " "$label" "$i"
        sleep 1
    done
    printf "\r  %s: готово        \n" "$label"
}

# Один тест: заголовок у лог, потім board.py із командою (якщо вона є), поки ви робите своє.
run_test() {
    local number="$1" title="$2" seconds="$3" command="$4" label="$5"
    rule
    bold "ТЕСТ $number — $title"
    {
        echo
        echo "=== ТЕСТ $number — $title — $(date '+%H:%M:%S') ==="
    } >> "$LOG"

    local args=(--for "$seconds" --log "$LOG")
    [[ -n "$command" ]] && args+=(--send "$command" --send-after 2)

    "$PY" "$ROOT/tools/board.py" "${args[@]}" > /dev/null 2>&1 &
    local watcher=$!
    sleep 2
    countdown "$((seconds - 3))" "$label"
    wait "$watcher" 2>/dev/null

    # Показати те, що плата сказала саме в цьому тесті — щоб було видно одразу, чи є сенс далі.
    tail -n 40 "$LOG" | grep -E '\[cal\]|\[heard\]|\[listen\]|\[state\]|\[vad\]' | tail -6 | sed 's/^/    /'
    echo
}

: > "$LOG"
{
    echo "RoboFace v1.4 — тести на платі"
    echo "дата: $(date '+%Y-%m-%d %H:%M:%S')"
    echo "гілка: $(git -C "$ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null)"
    echo "коміт: $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null)"
} >> "$LOG"

rule
bold "RoboFace v1.4 — шість тестів"
echo "  Лог: $LOG"
echo
echo "  Закрийте інші вікна board.py — порт тримає лише один процес."
echo "  Плата має бути ввімкнена і підключена до USB."
echo
read -r -p "  Enter — почати, Ctrl-C — вийти" _
echo

run_test 1 "рівень кімнати" 15 "/cal 10" "МОВЧІТЬ"
run_test 2 "звичайний голос" 15 "/cal 10" "ГОВОРІТЬ звичайно"
run_test 3 "тихий голос або здалеку" 15 "/cal 10" "ГОВОРІТЬ тихіше"

rule
bold "ТЕСТ 4 — hands-free оберт"
echo "  Нічого не натискайте. Скажіть одну фразу українською і замовкніть."
echo "  Слухайте, чи плата відповість УГОЛОС — це доведеться сказати словами."
run_test 4 "hands-free оберт" 30 "" "ГОВОРІТЬ фразу, потім тиша"

rule
bold "ТЕСТ 5 — хибні спрацювання"
echo "  Нічого не робіть і не говоріть. Дивимось, чи відкриється вікно саме."
run_test 5 "хибні спрацювання" 35 "" "ТИША"

rule
bold "ТЕСТ 6 — утримання екрана (запасний шлях)"
echo "  Притисніть і тримайте екран ~3 с, скажіть фразу, відпустіть."
run_test 6 "утримання екрана" 25 "" "ТРИМАЙТЕ і говоріть"

rule
bold "Готово."
echo
echo "  Надішліть цей файл: $LOG"
echo "  І одним рядком — чи почули ви відповідь уголос у тестах 4 і 6."
echo
grep -cE '\[cal\] кадр' "$LOG" | xargs printf "  вимірювань /cal у лозі: %s\n"
grep -cE '\[heard\]' "$LOG" | xargs printf "  розпізнаних фраз:       %s\n"
rule
