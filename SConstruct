#!/usr/bin/env python
import os
import sys

# You can find documentation for SCons and SConstruct files at:
# https://scons.org/documentation.html

# This lets SCons know that we're using godot-cpp, from the godot-cpp folder.
env = SConscript("godot-cpp/SConstruct")

# Configures the 'src' directory as a source for header files.
env.Append(CPPPATH=["src/", "thirdparty/zstd/lib/"])

# Collects all .cpp files in the 'src' folder as compile targets.
sources = Glob("src/*.cpp")
sources += Glob("thirdparty/zstd/lib/common/*.c")
sources += Glob("thirdparty/zstd/lib/compress/*.c")
sources += Glob("thirdparty/zstd/lib/decompress/*.c")

# The filename for the dynamic library for this GDExtension.
# $SHLIBPREFIX is a platform specific prefix for the dynamic library ('lib' on Unix, '' on Windows).
# $SHLIBSUFFIX is the platform specific suffix for the dynamic library (for example '.dll' on Windows).
# env["suffix"] includes the build's feature tags (e.g. '.windows.template_debug.x86_64')
# (see https://docs.godotengine.org/en/stable/tutorials/export/feature_tags.html).
# The final path should match a path in the '.gdextension' file.
name = "godot-sim-telemetry"
if env["target"] == "template_release":
    lib_filename = f"{env.subst('$SHLIBPREFIX')}simtelemetry{env.subst('$SHLIBSUFFIX')}"
else:
    lib_filename = f"{env.subst('$SHLIBPREFIX')}{name}{env['suffix']}{env.subst('$SHLIBSUFFIX')}"

if env["target"] in ["editor", "template_debug"]:
    doc_data = env.GodotCPPDocData("src/gen/doc_data.gen.cpp", source=Glob(f"demo/addons/{name}/docs/*.xml"))
    sources.append(doc_data)

# Creates a SCons target for the path with our sources.
library = env.SharedLibrary(
    "demo/addons/godot-sim-telemetry/bin/{}".format(lib_filename),
    source=sources,
)

default_targets = [library]

# move_to parameter
move_to = ARGUMENTS.get("move_to", "")
if move_to:
    def copy_to_target(target, source, env):
        import shutil
        import time
        src_addon_dir = os.path.abspath(f"demo/addons/{name}")
        dest_dir = os.path.abspath(move_to)
        
        print(f"Copying addon files to: {dest_dir}")
        if not os.path.exists(dest_dir):
            os.makedirs(dest_dir)
            
        def safe_copy_file(src, dst):
            if os.path.exists(dst):
                try:
                    os.remove(dst)
                except Exception:
                    pass
            try:
                shutil.copy2(src, dst)
            except PermissionError:
                old_dst = dst + f".{int(time.time())}.old"
                try:
                    os.rename(dst, old_dst)
                    shutil.copy2(src, dst)
                    print(f"Renamed locked file to '{os.path.basename(old_dst)}' and copied new file.")
                except Exception as e:
                    print(f"Failed to copy locked file '{dst}': {e}")
                    raise

        def safe_copy_dir(src, dst):
            if not os.path.exists(dst):
                os.makedirs(dst)
            for item in os.listdir(dst):
                if item.endswith(".old"):
                    try:
                        os.remove(os.path.join(dst, item))
                    except Exception:
                        pass
            
            for item in os.listdir(src):
                s = os.path.join(src, item)
                d = os.path.join(dst, item)
                if os.path.isdir(s):
                    safe_copy_dir(s, d)
                else:
                    safe_copy_file(s, d)

        # copy bin files
        src_folder = os.path.join(src_addon_dir, "bin")
        if os.path.exists(src_folder):
            dst_folder = os.path.join(dest_dir, "bin")
            safe_copy_dir(src_folder, dst_folder)
            print(f"Copied folder 'bin' to '{dst_folder}'")
                
        return None

    copy_target = env.Command("copy_addon", library, copy_to_target)
    AlwaysBuild(copy_target)
    default_targets.append(copy_target)

Default(default_targets)
