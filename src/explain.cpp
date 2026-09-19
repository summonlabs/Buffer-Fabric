#include "buffer_fabric/explain.hpp"

#include <cstdio>

namespace buffer_fabric {
namespace {

void append_u64(std::string* out, const char* label, u64 value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%s=%llu", label, static_cast<unsigned long long>(value));
    if (!out->empty()) out->push_back(' ');
    out->append(buffer);
}

}  // namespace

std::string Explanation::to_text() const {
    std::string out;
    char header[160];
    std::snprintf(header, sizeof(header), "pool=%llu gen=%llu epoch=%llu",
                  static_cast<unsigned long long>(pool.raw()),
                  static_cast<unsigned long long>(pool_generation.raw()),
                  static_cast<unsigned long long>(epoch.raw()));
    out.append(header);
    append_u64(&out, "raw", raw_units);
    append_u64(&out, "protected", protected_headroom_units);
    append_u64(&out, "usable", usable_units);
    append_u64(&out, "allocated", allocated_units);
    append_u64(&out, "reserved", reserved_units);
    append_u64(&out, "committed", committed_units);
    append_u64(&out, "pinned", pinned_units);
    append_u64(&out, "reclaimable", reclaimable_units);
    append_u64(&out, "free", free_units);
    append_u64(&out, "borrowed_in", borrowed_in_units);
    append_u64(&out, "lent_out", lent_out_units);
    append_u64(&out, "overcommit_used", overcommit_used_units);
    append_u64(&out, "overcommit_limit", overcommit_limit_units);
    append_u64(&out, "utilization_bp", utilization_bp);
    append_u64(&out, "binding_bp", binding_bp);
    out.append(" pressure=");
    out.append(std::string(to_string(pressure)));
    out.append(" binding=");
    out.append(std::string(to_string(binding)));
    append_u64(&out, "queues", queues.size());
    out.append(" closure=");
    out.append(accounting.closed ? "closed" : "VIOLATED");
    out.append(" authority=");
    out.append(authority.describe());
    out.append(" digest=");
    out.append(digest.to_hex());
    if (intent.any()) {
        out.append(" intent=");
        if (intent.reduce_required) {
            append_u64(&out, "reduce_to", intent.reduce_to_units);
        }
        if (intent.fence_required) out.append(" fence");
        if (intent.revalidate_required) out.append(" revalidate");
        if (intent.reclaim_target_units != 0) {
            append_u64(&out, "reclaim_target", intent.reclaim_target_units);
        }
    }
    return out;
}

void Explanation::write_json(JsonWriter& writer) const {
    writer.begin_object();
    writer.key("pool");
    writer.value_u64(pool.raw());
    writer.key("pool_generation");
    writer.value_u64(pool_generation.raw());
    writer.key("epoch");
    writer.value_u64(epoch.raw());
    writer.key("boot_hi");
    writer.value_u64(boot.hi);
    writer.key("boot_lo");
    writer.value_u64(boot.lo);
    writer.key("sequence");
    writer.value_u64(sequence);
    writer.key("generated_at");
    writer.value_u64(generated_at);

    writer.key("capacity");
    writer.begin_object();
    writer.key("raw");
    writer.value_u64(raw_units);
    writer.key("protected_headroom");
    writer.value_u64(protected_headroom_units);
    writer.key("usable");
    writer.value_u64(usable_units);
    writer.key("allocated");
    writer.value_u64(allocated_units);
    writer.key("reserved");
    writer.value_u64(reserved_units);
    writer.key("committed");
    writer.value_u64(committed_units);
    writer.key("pinned");
    writer.value_u64(pinned_units);
    writer.key("reclaimable");
    writer.value_u64(reclaimable_units);
    writer.key("free");
    writer.value_u64(free_units);
    writer.key("borrowed_in");
    writer.value_u64(borrowed_in_units);
    writer.key("lent_out");
    writer.value_u64(lent_out_units);
    writer.key("overcommit_used");
    writer.value_u64(overcommit_used_units);
    writer.key("overcommit_limit");
    writer.value_u64(overcommit_limit_units);
    writer.end_object();

    writer.key("pressure");
    writer.begin_object();
    writer.key("state");
    writer.value_string(to_string(pressure));
    writer.key("utilization_bp");
    writer.value_u64(utilization_bp);
    writer.key("binding_bp");
    writer.value_u64(binding_bp);
    writer.key("thresholds");
    writer.begin_object();
    writer.key("elevated_bp");
    writer.value_u64(thresholds.elevated_bp);
    writer.key("high_bp");
    writer.value_u64(thresholds.high_bp);
    writer.key("critical_bp");
    writer.value_u64(thresholds.critical_bp);
    writer.end_object();
    writer.key("evidence");
    writer.begin_object();
    writer.key("snapshot");
    writer.value_u64(evidence.snapshot.raw());
    writer.key("fresh");
    writer.value_bool(evidence.fresh);
    writer.key("epoch_match");
    writer.value_bool(evidence.epoch_match);
    writer.key("generation_match");
    writer.value_bool(evidence.generation_match);
    writer.key("age_ticks");
    writer.value_u64(evidence.age_ticks);
    writer.key("reason");
    writer.value_string(to_string(evidence.reason));
    writer.key("detail");
    writer.value_string(evidence.detail);
    writer.end_object();
    writer.end_object();

    writer.key("binding_constraint");
    writer.value_string(to_string(binding));
    writer.key("corrective_intent");
    writer.begin_object();
    writer.key("reduce_required");
    writer.value_bool(intent.reduce_required);
    writer.key("reduce_to");
    writer.value_u64(intent.reduce_to_units);
    writer.key("fence_required");
    writer.value_bool(intent.fence_required);
    writer.key("revalidate_required");
    writer.value_bool(intent.revalidate_required);
    writer.key("reclaim_target");
    writer.value_u64(intent.reclaim_target_units);
    writer.end_object();

    writer.key("authority");
    writer.begin_object();
    writer.key("digest");
    writer.value_string(authority.digest().to_hex());
    writer.key("entries");
    writer.begin_array();
    for (u8 i = 0; i < authority.count(); ++i) {
        writer.begin_object();
        writer.key("kind");
        writer.value_string(to_string(authority.at(i).kind));
        writer.key("value");
        writer.value_u64(authority.at(i).value);
        writer.key("generation");
        writer.value_u64(authority.at(i).generation.raw());
        writer.end_object();
    }
    writer.end_array();
    writer.end_object();

    writer.key("accounting_closed");
    writer.value_bool(accounting.closed);
    writer.key("accounting_violations");
    writer.value_u64(accounting.violations.size());

    writer.key("queues");
    writer.begin_array();
    for (const auto& usage : queues) {
        writer.begin_object();
        writer.key("queue");
        writer.value_u64(usage.queue.raw());
        writer.key("generation");
        writer.value_u64(usage.queue_generation.raw());
        writer.key("committed");
        writer.value_u64(usage.committed_units);
        writer.key("reserved");
        writer.value_u64(usage.reserved_units);
        writer.key("allocations");
        writer.value_u64(usage.allocation_count);
        writer.end_object();
    }
    writer.end_array();

    writer.key("digest");
    writer.value_string(digest.to_hex());
    writer.end_object();
}

std::string Explanation::to_json() const {
    JsonWriter writer(16384);
    write_json(writer);
    return writer.str();
}

std::string FabricSummary::to_json() const {
    JsonWriter writer(4096);
    writer.begin_object();
    writer.key("epoch");
    writer.value_u64(epoch.raw());
    writer.key("boot_hi");
    writer.value_u64(boot.hi);
    writer.key("boot_lo");
    writer.value_u64(boot.lo);
    writer.key("pool_count");
    writer.value_u64(pool_count);
    writer.key("queue_count");
    writer.value_u64(queue_count);
    writer.key("allocation_count");
    writer.value_u64(allocation_count);
    writer.key("live_allocations");
    writer.value_u64(live_allocations);
    writer.key("raw_total");
    writer.value_u64(raw_total);
    writer.key("protected_total");
    writer.value_u64(protected_total);
    writer.key("allocated_total");
    writer.value_u64(allocated_total);
    writer.key("free_total");
    writer.value_u64(free_total);
    writer.key("overcommit_total");
    writer.value_u64(overcommit_total);
    writer.key("borrowed_total");
    writer.value_u64(borrowed_total);
    writer.key("lent_total");
    writer.value_u64(lent_total);
    writer.key("accounting_closed");
    writer.value_bool(accounting.closed);
    writer.key("accounting_pools_checked");
    writer.value_u64(accounting.pools_checked);
    writer.key("accounting_violations");
    writer.value_u64(accounting.violations.size());
    writer.end_object();
    return writer.str();
}

}  // namespace buffer_fabric
