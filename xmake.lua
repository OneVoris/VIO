set_project("VIO")
set_version("0.1.0")
set_xmakever("3.0.0")
set_languages("cxx23")
set_warnings("allextra")
add_rules("mode.debug", "mode.release")

option("build_shared")
    set_default(false)
    set_showmenu(true)
option_end()

option("build_tests")
    set_default(false)
    set_showmenu(true)
option_end()

option("build_examples")
    set_default(false)
    set_showmenu(true)
option_end()

option("build_benchmarks")
    set_default(false)
    set_showmenu(true)
option_end()

option("build_fuzzers")
    set_default(false)
    set_showmenu(true)
option_end()

option("with_voris_dependencies")
    set_default(false)
    set_showmenu(true)
    set_description("Resolve released Voris dependencies through VXrepo")
option_end()

if has_config("with_voris_dependencies") then
    add_requires("voris-vmem >=0.1.0 <0.2.0")
end

target("voris_vio")
    if has_config("build_shared") then
        set_kind("shared")
    else
        set_kind("static")
    end
    add_headerfiles("include/(voris/**.hpp)")
    add_includedirs("include", {public = true})
    add_files("src/*.cpp")
    add_defines("VORIS_VIO_BUILD")

    if has_config("with_voris_dependencies") then
        add_packages("voris-vmem", {public = true})
    end

target_end()

if has_config("build_tests") then
    target("vio_smoke_test")
        set_kind("binary")
        add_files("tests/smoke.cpp")
        add_deps("voris_vio")
        add_tests("smoke")
    target_end()
end
