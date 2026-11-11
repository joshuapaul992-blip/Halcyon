#!/usr/bin/env python3
# placeholder, replaced later
import os, sys
out = sys.argv[1]
os.makedirs(out, exist_ok=True)
open(os.path.join(out, "icons.h"), "w").write("#ifndef GEN_ICONS_H\n#define GEN_ICONS_H\n#endif\n")
open(os.path.join(out, "icons.c"), "w").write("#include \"icons.h\"\nint icons_placeholder;\n")
