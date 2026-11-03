# Pokémon Violet (01008F6008C5E000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # Pokémon Violet 4.0.0 (v786432) main: nninitStartup (main+0x2b7da90, through its helper at main+0x167d510) initializes its StandardAllocator with virtual address memory over 12 GB of
  # address space (main+0x167d54c), which the Switch's 64 GB alias region holds and a phone's single 6 GB reservation cannot: the same call and the same cut as Pokémon Legends: Z-A's
  # (0100F43008C44000/patches.py). The pool bounds what the allocator maps (profile mem_map_limit)
  '709bfd66115298640155fcc4979dba15': [
    (0x167d54c, '6200c0d2', '2200c0d2', 'StandardAllocator::Initialize(nullptr, size, false): address space mov x2,#0x300000000 → mov x2,#0x100000000 (12 GB → 4 GB)'),
  ],
}

BINDING = [
    {'what': "StandardAllocator::Initialize's address space 12 GB → 4 GB (4.0.0 main)", 'now': 'build', 'goal': 'host', 'purpose': 'address space',
     'why': "the host's StandardAllocator gate lowers such a request in every build (docs/reference/memory.md, StandardAllocators over 12 to 62 GB of address space); "
            "Pokémon Legends: Z-A's same cut was removed once the gate played its route",
     'blocked': 'not yet run on 4.0.0 without the cut: kept as PR #269 tested it until a Mac run without it reaches the same frames'},
]
