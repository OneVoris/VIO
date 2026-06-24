set_project("VIO")
set_version("0.1.0")
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

add_requires("voris-vmem")

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
    add_packages("voris-vmem")

target_end()

if has_config("build_tests") then
    local test_sources = {
        smoke = "tests/smoke.cpp",
        error = "tests/error_test.cpp",
        scheduler = "tests/scheduler_test.cpp",
        task = "tests/task_test.cpp",
        task_lifetime = "tests/task_lifetime_test.cpp",
        test_scheduler = "tests/test_scheduler_test.cpp",
        virtual_clock = "tests/virtual_clock_test.cpp",
    }

    for name, source in pairs(test_sources) do
    target("vio_" .. name .. "_test")
        set_kind("binary")
        add_files(source)
        add_deps("voris_vio")
        add_tests(name)
    target_end()
    end
end
