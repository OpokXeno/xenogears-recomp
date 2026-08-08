set pagination off
set confirm off
set breakpoint pending on
set disable-randomization on
set print thread-events off

set $seal_count = 0
set $deferred_sealed = 0

break guest_render_transaction_seal_deferred_retry
commands
    silent
    set $seal_count = $seal_count + 1
    set $deferred_sealed = 1
    if $seal_count > 635
        printf "DEFERRED_SEAL count=%d serial=%llu\n", $seal_count, $rdi
    end
    continue
end

break guest_render_transaction_invalidate_deferred
commands
    silent
    if $deferred_sealed != 0
        if $seal_count > 635
            printf "DEFERRED_INVALIDATE count=%d\n", $seal_count
            bt 8
        end
        set $deferred_sealed = 0
    end
    continue
end

break guest_render_transaction_begin_deferred
commands
    silent
    printf "DEFERRED_BEGIN count=%d epoch=%llu sequence=%llu serial=%llu\n", $seal_count, $rdi, $rsi, $rdx
    bt 8
    continue
end

break guest_render_transaction_post_swap_success
commands
    silent
    printf "TRANSACTION_PUBLISH count=%d\n", $seal_count
    continue
end

break input_replay::write_evidence
commands
    silent
    printf "REPLAY_EVIDENCE count=%d\n", $seal_count
    bt 8
    print (int)g_guest_render_transaction_deferred_active
    print (int)display_disabled
    print (int)display_depth
    print (int)s_d24_present_hold
    quit
end

run
