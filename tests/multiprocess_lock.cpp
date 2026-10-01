#include "fel/fel.hpp"
#include "harness.hpp"
#include "support.hpp"

#include <string>

using namespace fel;
using namespace feltest;

namespace {

void create_ledger(const std::filesystem::path& directory) {
    const Result<Ledger> created =
        Ledger::create(directory, feltest::id<LedgerIdTag>("mp-ledger"), "fel-tests");
    if (!created.ok()) {
        throw feltest::Failure{"cannot create the ledger: " + created.reason().message()};
    }
}

}  // namespace

FEL_TEST(multiprocess, the_write_lock_is_enforced_by_the_kernel_across_processes) {
    feltest::TempDir directory("mp-lock");
    create_ledger(directory.path());
    const std::filesystem::path ready = directory.file("ready.txt");
    const std::filesystem::path release = directory.file("release.txt");
    const std::filesystem::path writer_result = directory.file("writer-result.txt");
    const std::filesystem::path reader_result = directory.file("reader-result.txt");

    const std::filesystem::path helper = feltest::helper_executable();
    feltest::ChildProcess holder =
        feltest::spawn_process(helper, {"hold-lock", directory.path().string(), ready.string(),
                                        release.string()});
    feltest::wait_for_file(ready, "the lock holder to report readiness");
    CHECK_EQ(feltest::read_text(ready), "held");

    CHECK_EQ(feltest::run_process(helper, {"try-writer-open", directory.path().string(),
                                           writer_result.string()}),
             0);
    const std::string writer_text = feltest::read_text(writer_result);
    CHECK(writer_text == "refused LockHeldExclusive");

    CHECK_EQ(feltest::run_process(helper, {"try-reader-open", directory.path().string(),
                                           reader_result.string()}),
             0);
    const std::string reader_text = feltest::read_text(reader_result);
    CHECK(reader_text == "refused LockHeldExclusive");

    feltest::write_text(release, "go");
    CHECK_EQ(feltest::wait_process(holder), 0);

    // Once the holder exits the lock is gone with the process.
    CHECK_EQ(feltest::run_process(helper, {"try-writer-open", directory.path().string(),
                                           writer_result.string()}),
             0);
    CHECK_EQ(feltest::read_text(writer_result), "opened");
}

FEL_TEST(multiprocess, an_abandoned_write_lock_does_not_survive_process_death) {
    feltest::TempDir directory("mp-abandon");
    create_ledger(directory.path());
    // The child acquires the write lock and is killed by exiting without
    // releasing anything.
    CHECK_EQ(feltest::run_process(feltest::helper_executable(),
                                  {"append-then-exit", directory.path().string(), "3"}),
             0);
    Result<Ledger> reopened = Ledger::open(directory.path(),
                                           StoreOptions{OpenMode::Writer, false, {}, {}, {}});
    REQUIRE(reopened.ok());
    CHECK_EQ(reopened.value().index().sources().size(), 3U);
    CHECK(!reopened.value().store().recovery().recovered_torn_tail);
}

FEL_TEST(multiprocess, work_committed_before_an_abrupt_exit_is_durable) {
    feltest::TempDir directory("mp-durable");
    create_ledger(directory.path());
    for (int round = 0; round < 3; ++round) {
        CHECK_EQ(feltest::run_process(
                     feltest::helper_executable(),
                     {"append-then-exit", directory.path().string(), "2",
                      "round-" + std::to_string(round)}),
                 0);
    }
    {
        Result<Ledger> reopened =
            Ledger::open(directory.path(), StoreOptions{OpenMode::Writer, false, {}, {}, {}});
        REQUIRE(reopened.ok());
        // Each round registered two sources, and the third round's writes are
        // present even though the process never unwound.
        CHECK_EQ(reopened.value().index().sources().size(), 6U);
    }
    const Result<IntegrityReport> integrity = verify_ledger_directory(directory.path());
    REQUIRE(integrity.ok());
    CHECK(integrity.value().ok);
    CHECK(!integrity.value().torn_tail);
}

FEL_TEST(multiprocess, independent_processes_fold_identical_state) {
    feltest::TempDir directory("mp-fold");
    create_ledger(directory.path());
    CHECK_EQ(feltest::run_process(feltest::helper_executable(),
                                  {"populate", directory.path().string()}),
             0);
    const std::filesystem::path first = directory.file("first.txt");
    const std::filesystem::path second = directory.file("second.txt");
    CHECK_EQ(feltest::run_process(feltest::helper_executable(),
                                  {"fold-digest", directory.path().string(), first.string()}),
             0);
    CHECK_EQ(feltest::run_process(feltest::helper_executable(),
                                  {"fold-digest", directory.path().string(), second.string()}),
             0);
    const std::string text = feltest::read_text(first);
    CHECK(text == feltest::read_text(second));
    CHECK(text.find("sources=1") != std::string::npos);
    CHECK(text.find("measurements=1") != std::string::npos);
    CHECK(text.find("allocations=1") != std::string::npos);
    CHECK(text.find("seals=1") != std::string::npos);

    // The parent folds the same journal and reaches the same revision.
    Result<Ledger> parent = Ledger::open(directory.path(),
                                         StoreOptions{OpenMode::Reader, false, {}, {}, {}});
    REQUIRE(parent.ok());
    CHECK(text.find("revision=" + std::to_string(parent.value().revision().value())) !=
          std::string::npos);
    CHECK(text.find("chain=" + parent.value().store().chain_head().to_hex()) != std::string::npos);
}
