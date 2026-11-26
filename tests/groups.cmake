# Every test belongs to exactly one ctest group (docs/building.md, "Test groups"):
#   quick    seconds-long logic and UI tests: the day-to-day run   ctest -L quick
#   full     slower tests that are not real-time                   ctest -LE release   (quick + full)
#   release  real-time playback, speed and timing tests, soak      ctest               (everything)
# Add a new test to one list below; check_test_groups (scripts/check_test_groups.py) fails otherwise.

# Real-time playback, speed / timing measurements, anything that fails under load or runs over a minute.
set(KK_TESTS_RELEASE
  test_cutperf test_themeswitch test_playbackgap test_playheadload test_slowmotionmemory
  test_playbackperf test_realworld test_wayland_session test_resizebench test_keyframecut
  test_memorysoak test_renderperf test_mediaperf test_proxyperf test_hevc4kperf test_compperf
  test_exportperf test_audioperf test_pageturnplayback test_cutlookahead test_editplaying
  test_firstplay test_uniqueframes test_timelinebench test_rendermemory test_playbackstats test_playbackstatslog test_interactionperf
  test_livepreview test_pasteaudiolive test_memoryprofile test_reverseplay
)

# Slower than about 5 s: whole-window pages, vim suites, stabilizer, process-level and render tests.
set(KK_TESTS_FULL
  test_vimcmdline test_vimkeys test_vimoption test_vimpanels test_vimmixer test_compositingpage
  test_stabilizer test_stabilizecommand test_stabilizejob test_stabilizeui test_stabilizerender
  test_livecontrol test_cutpage test_editkeys test_menulayout test_mainwindow test_singlefile
  test_menureference test_container test_polish2shots test_pagespring test_e2eflow
  test_bughunt12 test_inapphelp test_audiopage test_analyzerui test_trackeffectsui test_loudnessui test_trackers test_schemecolors
  test_embeddedmedia test_cli test_voiceover test_pagecommands test_openurl test_audiosync
  test_videodecoder test_polish4shots test_pointtracker test_mediamanagement test_commands
  test_adjustment test_inputlutui check_menu_reference test_pluginrender test_ofxrender
)

# Fast pure-logic and UI tests.
set(KK_TESTS_QUICK
  test_plugins test_plugincommands test_ofx test_audioplug test_mediametadatacsv test_lutpaths
  test_gallery test_autoalign test_textmesh3d test_inputlut test_metadatafull test_relinkfuzzy check_issue_forms check_status check_vim_docs check_translations test_actionregistry test_aimodeldialog
  test_animation test_audioclipeffects test_audioclipeffectsui test_trackeffects test_noisereduction test_noisereductionui test_audiodecoder test_audiomixer
  test_audionormalize test_automation test_loudness test_dynamicskey test_modfx test_spectrum test_monitorcontrols test_voiceisolation test_sends test_arabictext test_audiopitch test_audiorouting test_audiotransitions
  test_backgroundremoval test_bins test_birefnet test_boginspector test_bogplayback
  test_bogtimeline test_bughunt test_bughunt10 test_bughunt2 test_bughunt3 test_bughunt4
  test_bughunt5 test_bughunt6 test_bughunt6edit test_bughunt7 test_bughunt7edit test_bughunt8 test_multiedit test_bughunt13edit test_rippleaudit test_titledecor test_multiaudio
  test_bughunt9 test_bughuntkeys test_bugreport test_childprocessenv test_clipattributes
  test_clipeqeditor test_colorpage test_colorpageui test_commoncontrols test_compcolor test_compexport
  test_compfilters test_compositemode test_comptransition test_exr test_transitionpresets test_composition test_compositor test_compound test_comprender test_particles test_scene3d test_paint test_paintviewer
  test_compviewerview test_cutedits test_deliverpage test_dialogs test_dialogsizing test_dmabuf
  test_droptargets test_dynamiczoom test_dynamiczoomcontrols test_editaids test_editindex
  test_editops test_editops_ripple test_editops_timeline test_editpolish test_edlexport
  test_effectparams test_effects5 test_effectsb test_effectsgap test_effectsheets
  test_effectslibrary test_effectsui test_embeddedexport test_emoji test_encoderprobe
  test_export test_hdrexport test_hdrfile test_extrapolation test_fadecurves test_filedialogs test_folderimport
  test_folderscan test_fontreload test_formats test_framematch test_framerateconvert test_generators
  test_glviewer test_grade test_gradekeyframes test_gradeexport test_graderender test_guidedemo test_guideoverlay
  test_guidesrender test_homography test_hwdecode test_hwencprobe test_inspector test_keyframedcpu
  test_keyframeeditor test_keyframeops test_keyframespanel test_keypresets test_largeproject
  test_levellines test_levelops test_logexport test_welcome test_looks test_maskrender test_masks
  test_masksui test_mattefinesse test_mattemodel test_matteservice test_mcp test_mediacache test_mediahover
  test_medialinks test_mediaorganize test_mediapool test_mediapoolpanel test_membudget test_memoryguard
  test_memoryreport test_mergecontrols test_mixerdata test_mixerdsp test_mixergraph
  test_fusionclip test_fusionclipui test_multicam test_multicamrender test_multicamui test_nested test_nodeeditor test_otio
  test_panelscreens test_pagepanels test_parity test_parity_edit test_pasteaudio test_planartracker
  test_planartrackernode test_playbackengine test_playbacklogic test_playheadscrub test_polish2
  test_polish3 test_polish3shots test_polish4 test_poolfolders test_poolnavigation
  test_poolpaste test_preferences test_projectpresets test_probe test_project test_projectlibrary
  test_projectmanager test_projectpackage test_proxies test_rendercache test_renderdemo
  test_rendermath test_rendernotify test_retime test_retimebar test_retimerender test_robustness
  test_rvmbackend test_scopes test_serialization test_session test_sessionlog
  test_settingstransfer test_smartbins test_sourcetape test_stabilization test_stddefaults
  test_stddefaults_ui test_subtitleexport test_subtitles test_targetpool test_glapi test_textfit test_textplus test_compgroups test_modifiers test_trackfollow test_trackerpatterns test_theme test_uifonts test_tooltips
  test_thinprogressbar test_timecode test_timelineanimation test_timelinecontroller
  test_timelinedemo test_timelinedrop test_timelinefuzz test_timelineimport test_timelinelayout
  test_timelinetabs test_timelinewidget test_timestretch test_titleexport test_titlelayers
  test_titlelayersui test_trackernode test_transformcontrols test_transitions
  test_transitionselect test_translations test_trimtwoup test_undosnapshots test_varispeed
  test_videoeffects test_videoeffects2 test_viewerguides test_viewerpanel test_viewmenu
  test_vimforms test_vimgrammar test_vimkeyframes test_vimkeymap test_vimkeystab test_vimmacros
  test_vimnodes test_vimtransitions test_waveforms test_wayland test_windowtrack test_writeon
  test_kinoklipstyle test_outputblanking test_colorviewer
)
