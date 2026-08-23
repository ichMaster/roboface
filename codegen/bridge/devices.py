"""Device profiles — what a board is, expressed as data.

A profile is read by :func:`bridge.project.project`; nothing ever branches on a board
name. Adding a third board is therefore one more :class:`Profile` and no code change,
which is the property device-frontends-vision.md §10.4 promises.

The brightness ladder is **derived from the NOW poll interval**, never written as a
pair of constants. A ladder entry at index *i* takes effect after ``poll_s[NOW] * (i+1)``
seconds of no interaction, so retuning that interval carries the ladder with it instead
of leaving it quietly wrong (vision §4.4).

Stdlib only, though this package is not required to be — profiles are plain data and
there is nothing here worth a dependency.
"""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
from types import MappingProxyType

#: The notification channel. Not a screen: it carries what *happened*, where the
#: screens carry what *is* (vision §5.1).
WANT_NOTIFY = 0

SCREEN_NOW = 1
SCREEN_VELOCITY = 2
SCREEN_PLAN = 3
SCREEN_FRICTION = 4
SCREEN_ANALYTICS = 5
SCREEN_BURNDOWN = 6

#: Display names, indexed by ``want``. Used for logs and the golden-frame filenames,
#: never rendered — the device knows its own titles.
WANT_NAMES: Mapping[int, str] = MappingProxyType({
    WANT_NOTIFY: "notify",
    SCREEN_NOW: "now",
    SCREEN_VELOCITY: "velocity",
    SCREEN_PLAN: "plan",
    SCREEN_FRICTION: "friction",
    SCREEN_ANALYTICS: "analytics",
    SCREEN_BURNDOWN: "burndown",
})


@dataclass(frozen=True)
class Profile:
    """One board, as data.

    ``dim_ladder`` is brightness percentages ordered by idle step: index 0 is fresh,
    index 1 applies after twice the NOW interval, index 2 after three times it. The
    Core2's ends at 20 and never reaches 0 — that is what lets vision §1 keep claiming
    the panel is always visible. The StickC's ends at 0 because it is a pager.
    """

    name: str
    width: int
    height: int
    #: Characters that fit on one body-type line. The projection truncates to this,
    #: which is why a frame cannot outgrow the screen it feeds (vision §2.3).
    chars_per_line: int
    screens: tuple[int, ...]
    poll_s: Mapping[int, int]
    dim_ladder: tuple[int, ...]

    def has(self, want: int) -> bool:
        """Whether this board renders ``want`` at all."""
        return want == WANT_NOTIFY or want in self.screens

    def poll_for(self, want: int) -> int:
        """Seconds the device should wait before asking for ``want`` again."""
        return self.poll_s[want]

    def dim_at(self, idle_s: float) -> int:
        """Brightness for a device idle this long.

        Thresholds are multiples of the NOW interval, so this is the one place the
        ladder's *derivation* lives. A caller never supplies 30 or 45.
        """
        step = self.poll_s[SCREEN_NOW]
        level = self.dim_ladder[0]
        for index in range(1, len(self.dim_ladder)):
            if idle_s >= step * (index + 1):
                level = self.dim_ladder[index]
        return level


#: The desk display. Read, not felt — so it dims but never goes dark.
CORE2 = Profile(
    name="core2",
    width=320,
    height=240,
    chars_per_line=32,
    screens=(
        SCREEN_NOW, SCREEN_VELOCITY, SCREEN_PLAN,
        SCREEN_FRICTION, SCREEN_ANALYTICS, SCREEN_BURNDOWN,
    ),
    poll_s=MappingProxyType({
        WANT_NOTIFY: 5,       # the only channel that can buzz; the one real latency need
        SCREEN_NOW: 15,       # its clock moves; everything else is event-paced
        SCREEN_VELOCITY: 120,  # 30-minute buckets
        SCREEN_PLAN: 60,
        SCREEN_FRICTION: 60,
        SCREEN_ANALYTICS: 60,
        SCREEN_BURNDOWN: 60,
    }),
    dim_ladder=(100, 50, 20),
)

#: The pager. Felt, not read — so it sleeps, and its lines are shorter.
STICKC = Profile(
    name="stickc",
    width=240,
    height=135,
    chars_per_line=24,
    screens=(SCREEN_NOW,),
    poll_s=MappingProxyType({
        WANT_NOTIFY: 5,
        SCREEN_NOW: 15,
    }),
    dim_ladder=(100, 0),
)

#: The roster. A third board joins here and nowhere else.
PROFILES: Mapping[str, Profile] = MappingProxyType({p.name: p for p in (CORE2, STICKC)})
