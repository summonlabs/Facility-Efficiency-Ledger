add_executable(fel_tests
    harness.cpp
    unit_core.cpp
    unit_journal.cpp
    unit_store.cpp
    unit_view.cpp
    unit_reconcile.cpp
    unit_authority.cpp
    integration_ledger.cpp
    e2e_lifecycle.cpp
    property_random.cpp
    adversarial_journal.cpp
    adversarial_input.cpp
    multiprocess_lock.cpp
    restart_crash.cpp
    concurrency_threads.cpp
)
fel_apply_strict_warnings(fel_tests)
target_link_libraries(fel_tests PRIVATE fel Threads::Threads)
target_compile_definitions(fel_tests PRIVATE
    FEL_HELPER_PATH="$<TARGET_FILE:fel_test_helper>")

add_executable(fel_test_helper helper_child.cpp)
fel_apply_strict_warnings(fel_test_helper)
target_link_libraries(fel_test_helper PRIVATE fel Threads::Threads)

# The helper must sit next to the test binary, which is what the harness assumes.
add_dependencies(fel_tests fel_test_helper)

add_test(NAME fel.unit COMMAND fel_tests)
