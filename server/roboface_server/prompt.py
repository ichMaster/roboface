"""System-prompt assembly.

A pure function of its arguments -- no file reads, no configuration, no clock -- for the same
reason ``protocol.py`` is pure: a prompt builder that reaches for hidden inputs is one you
cannot reason about from its call site, and prompt regressions are already hard enough to see.

**The character here is a placeholder, and is marked as one.** ARCHITECTURE §The mind (v4)
gives RoboFace a canon file at ``ROBOFACE_CANON_PATH`` in **v4.1**; until then this is the
smallest thing that makes the model behave like a spoken companion rather than a chat
assistant. It is deliberately not an attempt at the real character -- writing one now would
mean writing it twice, and the second draft would have to argue with the first.

What is *not* placeholder is the shape: **v4.1 passes the canon in as an argument.** Keeping
this a pure function of what it is given is what makes that a one-line change rather than an
unpicking.
"""

from __future__ import annotations

from typing import Final

#: The stand-in character. **Superseded in v4.1** by the canon file
#: (``ROBOFACE_CANON_PATH``, ARCHITECTURE §Configuration and secrets). Short on purpose: every
#: token here is paid on every turn, and the real personality work belongs in v4.1 where it can
#: be edited without a deploy.
PLACEHOLDER_PERSONA: Final = (
    "Ти — RoboFace, настільний компаньйон з живим анімованим обличчям. "
    "Ти цікавий, теплий і трохи грайливий, але ніколи не набридливий."
)

#: Ukrainian is the product's language (MISSION), and the ASR and TTS seams are configured for
#: it end to end -- ``DEEPGRAM_LANGUAGE=uk`` from v1.3, a multilingual ElevenLabs voice from
#: v1.1. A reply in another language would arrive as audio the voice mispronounces.
LANGUAGE_INSTRUCTION: Final = (
    "Відповідай українською, якщо співрозмовник не звертається до тебе іншою мовою."
)

#: The instruction that matters most for a *spoken* character. A reply is heard, not read: the
#: listener cannot skim, and every extra sentence is latency they wait through. From v1 this
#: text is synthesized phrase by phrase, so length is time.
BREVITY_INSTRUCTION: Final = (
    "Відповідай коротко — одне-два речення, як у розмові вголос. "
    "Не використовуй списки, заголовки, емодзі чи розмітку: твою відповідь буде озвучено, "
    "а не прочитано."
)


def build_system_prompt(persona: str | None = None) -> str:
    """Assemble the system prompt for a turn.

    ``persona`` overrides :data:`PLACEHOLDER_PERSONA`. It is the seam v4.1 uses to pass the
    canon file in; until then every caller leaves it alone.
    """
    sections = (
        persona if persona is not None else PLACEHOLDER_PERSONA,
        LANGUAGE_INSTRUCTION,
        BREVITY_INSTRUCTION,
    )
    return "\n\n".join(section.strip() for section in sections if section.strip())
