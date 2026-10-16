# AMD native depth feedback trial

Backports the static feedback pipeline approach from KytyPS5/KytyPS5#1187, by overlordxrz-source (633696f09f7f9757ec70d67213281c4b44840e01), adapted to this fork's asynchronous pipeline compiler.

Drivers exposing VK_EXT_attachment_feedback_loop_layout without the dynamic-state extension now use native static feedback pipelines. The feedback state is included in pipeline equality and hashing. Drivers supporting both extensions keep dynamic state. Drivers supporting neither retain the existing depth-copy fallback.

Startup prints the selected path. On affected AMD drivers, expect layout=yes dynamic=no path=native-static. Existing saves and shader caches can be reused. This is a rendering correctness trial; neither a frame rate gain nor a fix for all geometry problems is established yet.

Validation: full Windows launcher build, program disk cache/resource tracking/settings tests, and executable --help smoke test in the accompanying workflow. Hardware image quality, GPU validation, and sustained gameplay still require testing. The inherited ShaderRecompilerComputeTests target is incompatible with this fork's APIs and is not built by this workflow.

Boot logo skipping is backported from upstream cc44a43ebc53be6c8df1574c65787858d404b5dc (#1127). The portable Play Wolverine.bat enables --skip-notice-screen true. This reports the console notice-screen skip flag to the game; upstream specifically documents Wolverine omitting movies/BootSequence/BootSequence.movie. This does not skip story cinematics. The general emulator and GUI option default to false; the Wolverine portable launcher opts in. No game files are modified.
