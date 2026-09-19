#include <cstdio>
#include <fstream>
#include <string>

#include "test_support.hpp"

using namespace buffer_fabric;
using bftest::Harness;
using bftest::TempDirectory;

namespace {

FabricConfig durable_config(const TempDirectory& directory, DurabilityMode mode) {
    FabricConfig config;
    config.persistence.directory = directory.path();
    config.persistence.mode = mode;
    return config;
}

std::string read_file(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(stream)),
                         std::istreambuf_iterator<char>());
    return contents;
}

void write_file(const std::string& path, const std::string& contents) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

void append_file(const std::string& path, const std::string& contents) {
    std::ofstream stream(path, std::ios::binary | std::ios::app);
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

/// Open a harness whose failure message names the exact error code.
void open_durable(Harness* harness, const TempDirectory& directory, const char* file, int line) {
    const Status status = harness->open(durable_config(directory, DurabilityMode::JournalSync));
    if (!status.ok()) {
        ::bftest::fail(file, line, "harness.open(durable)", status.to_string());
    }
}

}  // namespace

#define OPEN_DURABLE(harness, directory) open_durable(&(harness), (directory), __FILE__, __LINE__)

BF_TEST(persistence, journal_records_committed_allocations) {
    TempDirectory directory("journal");
    {
        Harness harness;
        OPEN_DURABLE(harness, directory);
        BF_REQUIRE(harness.observe_current().ok());
        auto decision = harness.allocate(40'000);
        BF_REQUIRE(decision.ok());
        BF_CHECK_EQ(decision.value().granted_units, u64{40'000});
        const auto journal = read_file(directory.file("fabric.bfj"));
        BF_CHECK(journal.size() > 32);
        BF_REQUIRE(harness.fabric.close().ok());
    }
    {
        Harness harness;
        OPEN_DURABLE(harness, directory);
        const RecoveryReport report = harness.fabric.recovery_report();
        BF_CHECK(report.durable_state_present);
        BF_CHECK(report.journal_loaded);
        BF_CHECK(report.journal_records_read > 0);
        BF_CHECK(report.journal_records_applied > 0);
        BF_CHECK_EQ(harness.own_usage(), u64{40'000});
        auto explanation = harness.fabric.explain(harness.pool);
        BF_REQUIRE(explanation.ok());
        BF_CHECK_EQ(explanation.value().allocated_units, u64{40'000});
        BF_CHECK(harness.fabric.validate_accounting().ok());
        BF_REQUIRE(harness.fabric.close().ok());
    }
}

BF_TEST(persistence, epoch_advances_on_every_reopen) {
    TempDirectory directory("epoch");
    EpochId first{};
    {
        Harness harness;
        OPEN_DURABLE(harness, directory);
        first = harness.fabric.epoch();
        BF_CHECK(first.valid());
        BF_REQUIRE(harness.fabric.close().ok());
    }
    {
        Harness harness;
        OPEN_DURABLE(harness, directory);
        BF_CHECK(harness.fabric.epoch().raw() > first.raw());
        const RecoveryReport report = harness.fabric.recovery_report();
        BF_CHECK_EQ(report.durable_epoch.raw(), first.raw());
        BF_CHECK(report.boot.valid());
        BF_CHECK(report.boot != report.previous_boot);
        BF_REQUIRE(harness.fabric.close().ok());
    }
    {
        Harness harness;
        OPEN_DURABLE(harness, directory);
        BF_CHECK(harness.fabric.epoch().raw() >= first.raw() + 2);
        BF_REQUIRE(harness.fabric.close().ok());
    }
}

BF_TEST(persistence, pressure_evidence_requires_revalidation_after_restart) {
    TempDirectory directory("pressure");
    {
        Harness harness;
        OPEN_DURABLE(harness, directory);
        BF_REQUIRE(harness.observe_current().ok());
        BF_CHECK(harness.pressure_state() == PressureState::Clear);
        BF_REQUIRE(harness.fabric.close().ok());
    }
    {
        Harness harness;
        OPEN_DURABLE(harness, directory);
        const RecoveryReport report = harness.fabric.recovery_report();
        BF_CHECK_EQ(report.pressure_snapshots_invalidated, u64{1});
        auto evidence = harness.fabric.pressure(harness.pool);
        BF_REQUIRE(evidence.ok());
        BF_CHECK(evidence.value().state == PressureState::Stale);
        BF_CHECK(evidence.value().reason == ErrorCode::StaleEpoch);
        // An increase is therefore refused until fresh evidence arrives.
        auto decision = harness.allocate(1000);
        BF_REQUIRE(decision.ok());
        BF_CHECK(decision.value().kind == DecisionKind::Revalidate);
        // Fresh evidence restores decidability.
        BF_REQUIRE(harness.observe_current().ok());
        auto granted = harness.allocate(1000);
        BF_REQUIRE(granted.ok());
        BF_CHECK_EQ(granted.value().granted_units, u64{1000});
        BF_REQUIRE(harness.fabric.close().ok());
    }
}

BF_TEST(persistence, reservations_do_not_survive_restart) {
    TempDirectory directory("reservation");
    {
        Harness harness;
        OPEN_DURABLE(harness, directory);
        BF_REQUIRE(harness.observe_current().ok());
        AllocateRequest request = harness.allocate_request(5000);
        request.commit_immediately = false;
        BF_REQUIRE(harness.fabric.allocate(request).ok());
        BF_CHECK_EQ(harness.own_usage(), u64{5000});
        BF_REQUIRE(harness.fabric.close().ok());
    }
    {
        Harness harness;
        OPEN_DURABLE(harness, directory);
        const RecoveryReport report = harness.fabric.recovery_report();
        BF_CHECK_EQ(report.unfinished_attempts.size(), usize{1});
        BF_CHECK_EQ(report.unfinished_attempts[0].units, u64{5000});
        BF_CHECK_EQ(report.allocations_restored_as_uncommitted, u64{1});
        BF_CHECK_EQ(harness.own_usage(), u64{0});
        BF_CHECK(harness.fabric.validate_accounting().ok());
        BF_REQUIRE(harness.fabric.close().ok());
    }
}

BF_TEST(persistence, checkpoint_writes_a_snapshot_and_truncates_the_journal) {
    TempDirectory directory("checkpoint");
    {
        Harness harness;
        OPEN_DURABLE(harness, directory);
        BF_REQUIRE(harness.observe_current().ok());
        BF_REQUIRE(harness.allocate(1000).ok());
        BF_REQUIRE(harness.fabric.checkpoint().ok());
        const auto snapshot = read_file(directory.file("fabric.bfs"));
        BF_CHECK(snapshot.size() > 32);
        const auto journal = read_file(directory.file("fabric.bfj"));
        BF_CHECK(journal.empty());
        BF_REQUIRE(harness.fabric.close().ok());
    }
    {
        Harness harness;
        OPEN_DURABLE(harness, directory);
        const RecoveryReport report = harness.fabric.recovery_report();
        BF_CHECK(report.snapshot_loaded);
        BF_CHECK(report.snapshot_integrity_verified);
        BF_CHECK(report.journal_loaded == false);
        BF_CHECK_EQ(harness.own_usage(), u64{1000});
        BF_CHECK(harness.fabric.validate_accounting().ok());
        BF_REQUIRE(harness.fabric.close().ok());
    }
}

BF_TEST(persistence, torn_journal_tail_is_discarded_and_reported) {
    TempDirectory directory("torn");
    {
        Harness harness;
        OPEN_DURABLE(harness, directory);
        BF_REQUIRE(harness.observe_current().ok());
        auto first = harness.allocate(1000);
        BF_REQUIRE(first.ok());
        BF_CHECK_EQ(first.value().granted_units, u64{1000});
        BF_REQUIRE(harness.observe_current().ok());
        auto second = harness.allocate(2000);
        BF_REQUIRE(second.ok());
        BF_CHECK_EQ(second.value().granted_units, u64{2000});
        BF_REQUIRE(harness.fabric.close().ok());
    }
    const std::string journal_path = directory.file("fabric.bfj");
    const std::string original = read_file(journal_path);
    BF_REQUIRE(!original.empty());
    // Simulate an interrupted append: a partial record with a valid-looking
    // header but a payload that stops in the middle.
    append_file(journal_path, original.substr(0, 24));
    {
        Harness harness;
        OPEN_DURABLE(harness, directory);
        const RecoveryReport report = harness.fabric.recovery_report();
        BF_CHECK(report.outcome == RecoveryOutcome::TailTruncated);
        BF_CHECK(report.journal_bytes_discarded > 0);
        BF_CHECK(!report.warnings.empty());
        BF_CHECK_EQ(harness.own_usage(), u64{3000});
        BF_CHECK(harness.fabric.validate_accounting().ok());
        // The journal was repaired, so the next reopen is clean.
        BF_REQUIRE(harness.fabric.close().ok());
    }
    {
        Harness harness;
        OPEN_DURABLE(harness, directory);
        BF_CHECK(harness.fabric.recovery_report().outcome != RecoveryOutcome::TailTruncated);
        BF_CHECK_EQ(harness.own_usage(), u64{3000});
        BF_REQUIRE(harness.fabric.close().ok());
    }
}

BF_TEST(persistence, mid_file_corruption_is_refused) {
    TempDirectory directory("midfile");
    {
        Harness harness;
        OPEN_DURABLE(harness, directory);
        for (u64 units = 1000; units <= 3000; units += 1000) {
            BF_REQUIRE(harness.observe_current().ok());
            auto decision = harness.allocate(units);
            BF_REQUIRE(decision.ok());
            BF_CHECK_EQ(decision.value().granted_units, units);
        }
        BF_REQUIRE(harness.fabric.close().ok());
    }
    const std::string path = directory.file("fabric.bfj");
    std::string journal = read_file(path);
    BF_REQUIRE(journal.size() > 200);
    // Flip a byte inside the first record's payload, leaving intact records
    // after it. That is corruption, not a torn tail, and must be refused.
    journal[40] = static_cast<char>(journal[40] ^ 0x5A);
    write_file(path, journal);
    Harness harness;
    const Status status = harness.open(durable_config(directory, DurabilityMode::JournalSync));
    BF_CHECK_CODE(status, ErrorCode::CorruptRecord);
}

BF_TEST(persistence, corrupt_snapshot_is_refused) {
    TempDirectory directory("corruptsnapshot");
    {
        Harness harness;
        OPEN_DURABLE(harness, directory);
        BF_REQUIRE(harness.observe_current().ok());
        BF_REQUIRE(harness.allocate(1000).ok());
        BF_REQUIRE(harness.fabric.checkpoint().ok());
        BF_REQUIRE(harness.fabric.close().ok());
    }
    const std::string path = directory.file("fabric.bfs");
    std::string snapshot = read_file(path);
    BF_REQUIRE(snapshot.size() > 64);
    snapshot[snapshot.size() / 2] = static_cast<char>(snapshot[snapshot.size() / 2] ^ 0xFF);
    write_file(path, snapshot);
    Harness harness;
    const Status status = harness.open(durable_config(directory, DurabilityMode::JournalSync));
    BF_CHECK(!status.ok());
    BF_CHECK(status.code() == ErrorCode::IntegrityFailure ||
             status.code() == ErrorCode::CorruptRecord);
}

BF_TEST(persistence, truncated_snapshot_is_refused) {
    TempDirectory directory("truncatedsnapshot");
    {
        Harness harness;
        OPEN_DURABLE(harness, directory);
        BF_REQUIRE(harness.observe_current().ok());
        BF_REQUIRE(harness.allocate(1000).ok());
        BF_REQUIRE(harness.fabric.checkpoint().ok());
        BF_REQUIRE(harness.fabric.close().ok());
    }
    const std::string path = directory.file("fabric.bfs");
    const std::string snapshot = read_file(path);
    write_file(path, snapshot.substr(0, snapshot.size() / 2));
    Harness harness;
    const Status status = harness.open(durable_config(directory, DurabilityMode::JournalSync));
    BF_CHECK(!status.ok());
}

BF_TEST(persistence, snapshot_version_mismatch_is_refused) {
    TempDirectory directory("version");
    {
        Harness harness;
        OPEN_DURABLE(harness, directory);
        BF_REQUIRE(harness.fabric.checkpoint().ok());
        BF_REQUIRE(harness.fabric.close().ok());
    }
    const std::string path = directory.file("fabric.bfs");
    std::string snapshot = read_file(path);
    BF_REQUIRE(snapshot.size() > 16);
    snapshot[4] = static_cast<char>(0x7F);
    write_file(path, snapshot);
    Harness harness;
    const Status status = harness.open(durable_config(directory, DurabilityMode::JournalSync));
    BF_CHECK(!status.ok());
}

BF_TEST(persistence, in_memory_mode_makes_no_durability_claim) {
    TempDirectory directory("memory");
    {
        Harness harness;
        BF_REQUIRE(harness.open().ok());
        BF_REQUIRE(harness.observe_current().ok());
        BF_REQUIRE(harness.allocate(1000).ok());
        const RecoveryReport report = harness.fabric.recovery_report();
        BF_CHECK(!report.durable_state_present);
        BF_CHECK_EQ(report.outcome, RecoveryOutcome::Created);
    }
    BF_CHECK(read_file(directory.file("fabric.bfj")).empty());
}

BF_TEST(persistence, reopen_restores_topology_and_policy) {
    TempDirectory directory("topology");
    {
        Harness harness;
        OPEN_DURABLE(harness, directory);
        BF_REQUIRE(harness.fabric.close().ok());
    }
    Harness harness;
    harness.fabric.set_clock(&harness.clock);
    OPEN_DURABLE(harness, directory);
    auto policy = harness.fabric.policy(harness.policy_id);
    BF_REQUIRE(policy.ok());
    BF_CHECK_EQ(policy.value().name, std::string("harness"));
    auto pool = harness.fabric.pool_view(harness.pool);
    BF_REQUIRE(pool.ok());
    BF_CHECK_EQ(pool.value().descriptor.raw_units, bftest::kPoolRaw);
    BF_CHECK_EQ(pool.value().descriptor.protected_units, bftest::kPoolProtected);
    BF_CHECK(harness.fabric.validate_accounting().ok());
    BF_REQUIRE(harness.fabric.close().ok());
}

BF_TEST(persistence, recovery_report_is_json_encodable) {
    TempDirectory directory("report");
    Harness harness;
    OPEN_DURABLE(harness, directory);
    const std::string text = harness.fabric.recovery_report().to_json();
    BF_CHECK(text.find("\"outcome\"") != std::string::npos);
    BF_CHECK(text.find("\"journal_records_applied\"") != std::string::npos);
    BF_REQUIRE(harness.fabric.close().ok());
}
