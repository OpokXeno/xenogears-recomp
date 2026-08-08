set pagination off
set confirm off
set print thread-events off
set $last_stage_index = 0
set $last_stage_packet = 0
set $last_stage_primitive = 0

break world_models_stage_primitive
commands
    silent
    set $last_stage_index = $rcx
    set $last_stage_packet = $rdx
    set $last_stage_primitive = $rsi
    continue
end

break guest_render_bridge_abort_scene
condition 2 $rdi == 1
commands
    silent
    printf "FORCED_ORIGINAL stage=%u packet=0x%x primitive=0x%lx stage_detail=%u commit_detail=%u\\n", $last_stage_index, $last_stage_packet, $last_stage_primitive, *(unsigned int *)0x3b192280, *(unsigned int *)0x3a9a7184
    printf "counts initial=%u actual=%u expected=%u counter=%u accepted=%u\\n", *(unsigned int *)0x3a9a7178, *(unsigned int *)0x3a9a7180, *(unsigned int *)0x3a9a717c, *(unsigned int *)0x3a9a7174, *(unsigned int *)0x3a9a7170
    printf "guest right=%u bottom=0x%x native_margin=%d\\n", *(unsigned int *)0x3a9a70d0, *(unsigned int *)0x3a9a70cc, *(int *)0x3a9a70c8
    printf "no_counter screen=%u projection=%u nclip=%u\\n", *(unsigned int *)0x3a9a7164, *(unsigned int *)0x3a9a716c, *(unsigned int *)0x3a9a7168
    printf "first_screen index=%u family=%u packet=0x%x bottom=0x%x right=%u xy=%x,%x,%x,%x guest=%x,%x,%x\\n", *(unsigned int *)0x3a9a7118, *(unsigned int *)0x3a9a7114, *(unsigned int *)0x3a9a7110, *(unsigned int *)0x3a9a70d8, *(unsigned int *)0x3a9a70d4, *(unsigned int *)0x3a9a7100, *(unsigned int *)0x3a9a7104, *(unsigned int *)0x3a9a7108, *(unsigned int *)0x3a9a710c, *(unsigned int *)0x3a9a70e0, *(unsigned int *)0x3a9a70e4, *(unsigned int *)0x3a9a70e8
    bt 20
    detach
    quit
end

continue
