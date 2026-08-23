"""Unit tests for system-prompt assembly.

The prompt is an input to every turn, so the properties worth pinning are the ones whose
breakage is invisible: that it is deterministic, that it reads nothing it was not given, and
that the three instructions the spoken character depends on are actually in it.
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

from roboface_server.prompt import (
    BREVITY_INSTRUCTION,
    LANGUAGE_INSTRUCTION,
    PLACEHOLDER_PERSONA,
    build_system_prompt,
)


def test_assembly_is_deterministic() -> None:
    assert build_system_prompt() == build_system_prompt()


def test_the_prompt_carries_persona_language_and_brevity() -> None:
    prompt = build_system_prompt()

    assert PLACEHOLDER_PERSONA in prompt
    assert LANGUAGE_INSTRUCTION in prompt
    assert BREVITY_INSTRUCTION in prompt


def test_the_brevity_instruction_forbids_markup() -> None:
    # The reply is synthesized, not rendered. A bulleted list becomes a voice reading
    # punctuation aloud, and a heading becomes a sentence nobody wrote.
    assert "списки" in BREVITY_INSTRUCTION
    assert "озвучено" in BREVITY_INSTRUCTION


def test_the_persona_is_overridable() -> None:
    # The seam v4.1 uses to pass the canon file in, exercised now so it cannot rot.
    prompt = build_system_prompt(persona="Ти — інший персонаж.")

    assert "Ти — інший персонаж." in prompt
    assert PLACEHOLDER_PERSONA not in prompt
    # The non-persona instructions are not the canon's to replace.
    assert LANGUAGE_INSTRUCTION in prompt
    assert BREVITY_INSTRUCTION in prompt


def test_an_empty_persona_leaves_no_blank_section() -> None:
    prompt = build_system_prompt(persona="   ")

    assert "\n\n\n" not in prompt
    assert prompt.startswith(LANGUAGE_INSTRUCTION)


def test_the_placeholder_is_marked_as_superseded_by_v4_1(repo_root: Path) -> None:
    """A reader must not mistake the stub for the intended character.

    Asserted rather than trusted to review: this constant will be read by whoever writes v4.1,
    and its status is the first thing they need to know.
    """
    source = (repo_root / "server" / "roboface_server" / "prompt.py").read_text(encoding="utf-8")

    assert "v4.1" in source
    assert "ROBOFACE_CANON_PATH" in source


def test_the_module_reads_nothing_it_was_not_given(repo_root: Path) -> None:
    """Purity, checked by import rather than by inspection.

    A prompt builder that quietly reads a file or an environment variable is one whose output
    depends on the machine it runs on -- and prompt regressions are hard enough to see without
    that. A fresh interpreter with a poisoned environment must produce the same string.
    """
    program = (
        "import sys;"
        "import roboface_server.prompt as p;"
        "banned = sorted(m for m in sys.modules "
        "if m.split('.')[0] in {'dotenv', 'fastapi', 'websockets'});"
        "print(repr(banned) + '|' + p.build_system_prompt())"
    )
    environment = dict(os.environ)
    environment["PYTHONPATH"] = "server"
    environment["ROBOFACE_CANON_PATH"] = "/nonexistent/canon.md"
    environment["GEMINI_API_KEY"] = "should-not-be-read"

    result = subprocess.run(
        [sys.executable, "-c", program],
        cwd=repo_root,
        capture_output=True,
        text=True,
        env=environment,
        timeout=120,
    )

    assert result.returncode == 0, result.stdout + result.stderr
    imported, prompt = result.stdout.strip().split("|", 1)
    assert imported == "[]", f"prompt.py pulled in an I/O module: {imported}"
    # Same string despite a poisoned ROBOFACE_CANON_PATH and a GEMINI_API_KEY in the
    # environment: neither was read, because neither was passed.
    assert prompt == build_system_prompt()
    assert "should-not-be-read" not in prompt
