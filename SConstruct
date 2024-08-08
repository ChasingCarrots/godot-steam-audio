#!/usr/bin/env python

env = SConscript("src/lib/godot-cpp/SConstruct")

env.Append(CPPPATH=["src/"])

if env.get("CC", "").lower() == "cl":
    # Building with MSVC
    env.AppendUnique(CCFLAGS=("/I",  "src/lib/steamaudio/unity/include/phonon/"))
else:
    env.AppendUnique(CCFLAGS=("-isystem",  "src/lib/steamaudio/unity/include/phonon/"))

opts = Variables(None, ARGUMENTS)
opts.Add(BoolVariable("profiling_enabled", "Build with profiling active", False))
opts.Update(env)

sources = Glob("src/*.cpp")

steam_audio_lib_path = env.get("STEAM_AUDIO_LIB_PATH", "src/lib/steamaudio/lib")
tracy_lib_path = env.get("TRACY_LIB_PATH", 'src/lib/tracy')

if env["platform"] == "linux":
    env.Append(LIBPATH=[f'{steam_audio_lib_path}/linux-x64'])
    env.Append(LIBS=["libphonon.so"])
elif env["platform"] == "windows":
    env.Append(LIBPATH=[f'{steam_audio_lib_path}/windows-x64', f'{tracy_lib_path}/library/win32'])
    env.Append(LIBS=["phonon", "TracyProfiler"])
    if env["profiling_enabled"]:
        env.Append(LIBPATH=[f'{tracy_lib_path}/library/win32'])
        env.Append(LIBS=["TracyProfiler"])
        env.Append(CPPDEFINES=["PROFILING_ENABLED"])
        env.Append(CPPDEFINES=["TRACY_ENABLE"])
        env.Append(CPPDEFINES=["TRACY_ON_DEMAND"])
        env.Append(CPPDEFINES=["TRACY_IMPORTS"])
        env.Prepend(CPPPATH=f'{tracy_lib_path}/public')
elif env["platform"] == "macos":
    env.Append(LIBPATH=[f'{steam_audio_lib_path}/osx'])
    env.Append(LIBS=["libphonon.dylib"])

library = env.SharedLibrary(
    "project/addons/godot-steam-audio/bin/godot-steam-audio{}{}".format(env["suffix"], env["SHLIBSUFFIX"]),
    source=sources,
)

Default(library)
