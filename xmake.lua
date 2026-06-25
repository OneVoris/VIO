set_project("VIO")
set_version("0.1.0")
set_languages("cxx23")
set_warnings("allextra")
add_rules("mode.debug", "mode.release")
add_repositories("vxrepo https://github.com/OneVoris/VXrepo.git")

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

option("sanitize_thread")
    set_default(false)
    set_showmenu(true)
    set_description("Enable ThreadSanitizer on supported Linux clang/gcc-like toolchains.")
option_end()

add_requires("voris-vmem")

if has_config("sanitize_thread") then
    if is_plat("linux") then
        add_cxflags("-fsanitize=thread", "-fno-omit-frame-pointer", {force = true})
        add_ldflags("-fsanitize=thread", {force = true})
    end
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
    add_packages("voris-vmem")

target_end()

if has_config("build_tests") then
    local test_sources = {
        smoke = "tests/smoke.cpp",
        async_scope = "tests/async_scope_test.cpp",
        async_mutex = "tests/async_mutex_test.cpp",
        async_semaphore = "tests/async_semaphore_test.cpp",
        backend_contract = "tests/backend_contract_test.cpp",
        backend_differential = "tests/backend_differential_test.cpp",
        blocking_executor = "tests/blocking_executor_test.cpp",
        bounded_queue = "tests/bounded_queue_test.cpp",
        cancellation = "tests/cancellation_test.cpp",
        cancellation_stress = "tests/cancellation_stress_test.cpp",
        channel = "tests/channel_test.cpp",
        compute_executor = "tests/compute_executor_test.cpp",
        deadline = "tests/deadline_test.cpp",
        error = "tests/error_test.cpp",
        file_io = "tests/file_io_test.cpp",
        hardening_stress = "tests/hardening_stress_test.cpp",
        mailbox = "tests/mailbox_test.cpp",
        native_handle_registry = "tests/native_handle_registry_test.cpp",
        epoll_backend = "tests/epoll_backend_test.cpp",
        io_uring_backend = "tests/io_uring_backend_test.cpp",
        iocp_backend = "tests/iocp_backend_test.cpp",
        kqueue_backend = "tests/kqueue_backend_test.cpp",
        e2e_overload = "tests/e2e_overload_test.cpp",
        runtime_builder = "tests/runtime_builder_test.cpp",
        runtime_metrics = "tests/runtime_metrics_test.cpp",
        scheduler = "tests/scheduler_test.cpp",
        shard_lifecycle = "tests/shard_lifecycle_test.cpp",
        shard_runtime_integration = "tests/shard_runtime_integration_test.cpp",
        socket_io = "tests/socket_io_test.cpp",
        task = "tests/task_test.cpp",
        task_lifetime = "tests/task_lifetime_test.cpp",
        test_scheduler = "tests/test_scheduler_test.cpp",
        timer = "tests/timer_test.cpp",
        submit_to = "tests/submit_to_test.cpp",
        virtual_clock = "tests/virtual_clock_test.cpp",
        manual_reset_event = "tests/manual_reset_event_test.cpp",
        wakeup_budget = "tests/wakeup_budget_test.cpp",
        when_all = "tests/when_all_test.cpp",
        when_any = "tests/when_any_test.cpp",
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

if has_config("build_benchmarks") then
    local benchmark_sources = {
        backend_ping_pong = "benchmarks/backend_ping_pong_benchmark.cpp",
        timer_heap = "benchmarks/timer_heap_benchmark.cpp",
        scheduler = "benchmarks/scheduler_benchmark.cpp",
        channel = "benchmarks/channel_benchmark.cpp",
    }

    for name, source in pairs(benchmark_sources) do
    target("vio_" .. name .. "_benchmark")
        set_kind("binary")
        add_files(source)
        add_deps("voris_vio")
    target_end()
    end
end

if has_config("build_examples") then
    local example_sources = {
        echo = "examples/echo.cpp",
        fan_out = "examples/fan_out.cpp",
        file_copy = "examples/file_copy.cpp",
        graceful_shutdown = "examples/graceful_shutdown.cpp",
    }

    for name, source in pairs(example_sources) do
    target("vio_example_" .. name)
        set_kind("binary")
        add_files(source)
        add_deps("voris_vio")
    target_end()
    end
end
