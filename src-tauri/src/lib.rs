pub mod core;
pub mod file;
pub mod global_state;
pub mod hot_load_page;
pub mod gemini;
pub mod http_services;
pub mod listen_key;
pub mod ocr;
pub mod plugin;
pub mod screenshot;
pub mod scroll_screenshot;
pub mod video_record;
pub mod webview;

use crate::gemini::GeminiRequestManager;
use snow_shot_app_services::listen_mouse_service;
use snow_shot_tauri_commands_core::{FullScreenDrawWindowLabels, VideoRecordWindowLabels};
use std::sync::Arc;
use tauri::Emitter;
use tokio::sync::Mutex;

use tauri::Manager;

use snow_shot_app_os::ui_automation::UIElements;
use snow_shot_app_scroll_screenshot_service::scroll_screenshot_capture_service;
use snow_shot_app_scroll_screenshot_service::scroll_screenshot_image_service;
use snow_shot_app_scroll_screenshot_service::scroll_screenshot_service;
use snow_shot_app_services::file_cache_service;
use snow_shot_app_services::free_drag_window_service;
use snow_shot_app_services::hot_load_page_service;
use snow_shot_app_services::listen_key_service;
use snow_shot_app_services::ocr_service::OcrService;
use snow_shot_app_services::resize_window_service;
use snow_shot_app_services::video_record_service;
use snow_shot_app_shared::EnigoManager;
use snow_shot_global_state::{CaptureState, ReadClipboardState, WebViewSharedBufferState};
use snow_shot_plugin_service::plugin_service;

#[cfg(feature = "dhat-heap")]
pub static PROFILER: std::sync::LazyLock<Mutex<Option<dhat::Profiler>>> =
    std::sync::LazyLock::new(|| Mutex::new(None));

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    let ocr_instance = Mutex::new(OcrService::new());
    let video_record_service = Mutex::new(video_record_service::VideoRecordService::new());
    let hot_load_page_service = Arc::new(hot_load_page_service::HotLoadPageService::new());
    let enigo_instance = Mutex::new(EnigoManager::new());

    let ui_elements = Mutex::new(UIElements::new());

    let scroll_screenshot_service =
        Mutex::new(scroll_screenshot_service::ScrollScreenshotService::new());
    let scroll_screenshot_image_service =
        Mutex::new(scroll_screenshot_image_service::ScrollScreenshotImageService::new());
    let scroll_screenshot_capture_service =
        Mutex::new(scroll_screenshot_capture_service::ScrollScreenshotCaptureService::new());
    #[cfg(target_os = "windows")]
    let shared_buffer_service = Arc::new(snow_shot_webview::SharedBufferService::new());

    let free_drag_window_service =
        Mutex::new(free_drag_window_service::FreeDragWindowService::new());
    let resize_window_service = Mutex::new(resize_window_service::ResizeWindowService::new());

    let listen_key_service = Mutex::new(listen_key_service::ListenKeyService::new());
    let listen_mouse_service = Mutex::new(listen_mouse_service::ListenMouseService::new());

    let file_cache_service = Arc::new(file_cache_service::FileCacheService::new());

    let enable_run_log = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
    let enable_run_log_clone = enable_run_log.clone();

    let plugin_service = Arc::new(plugin_service::PluginService::new());

    let capture_state = Mutex::new(CaptureState { capturing: false });
    let gemini_request_manager = GeminiRequestManager::default();

    let full_screen_draw_window_labels = Mutex::new(Option::<FullScreenDrawWindowLabels>::None);
    let video_record_window_label = Mutex::new(Option::<VideoRecordWindowLabels>::None);

    let webview_shared_buffer_state = WebViewSharedBufferState::new(false);

    let read_clipboard_state = Mutex::new(ReadClipboardState { reading: false });

    use tauri_plugin_log::{Target, TargetKind};

    // let current_date = chrono::Local::now().format("%Y-%m-%d").to_string();

    // log 文件可能因为某些异常情况不断输出，造成日志文件过大
    // 先在 release 下屏蔽日志输出
    // 注意不要移除 log 插件的初始化,避免前端调用 log 时保存再次报错,持续循环报错
    let log_targets: Vec<Target> = if cfg!(debug_assertions) {
        vec![
            Target::new(TargetKind::Stdout),
            Target::new(TargetKind::LogDir { file_name: None }),
            Target::new(TargetKind::Webview),
        ]
    } else {
        vec![Target::new(TargetKind::LogDir { file_name: None })]
    };
    let log_level = if cfg!(debug_assertions) {
        log::LevelFilter::Debug
    } else {
        log::LevelFilter::Info
    };

    #[allow(unused_mut)]
    let mut app_builder = tauri::Builder::default()
        .plugin(tauri_plugin_updater::Builder::new().build())
        .plugin(
            tauri_plugin_window_state::Builder::new()
                .with_state_flags(
                    tauri_plugin_window_state::StateFlags::SIZE
                        | tauri_plugin_window_state::StateFlags::POSITION,
                )
                .with_filter(|label| label == "main")
                .build(),
        )
        .plugin(tauri_plugin_os::init())
        .plugin(tauri_plugin_single_instance::init(|app, _, _| {
            let app_window = app.get_webview_window("main").expect("no main window");
            app_window.show().unwrap();
            app_window.unminimize().unwrap();
            app_window.set_focus().unwrap();
        }))
        .plugin(tauri_plugin_macos_permissions::init())
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_clipboard::init())
        .plugin(tauri_plugin_clipboard_manager::init())
        .plugin(tauri_plugin_store::Builder::new().build())
        .plugin(tauri_plugin_http::init())
        .plugin(tauri_plugin_autostart::init(
            tauri_plugin_autostart::MacosLauncher::LaunchAgent,
            Some(vec!["--auto_start"]),
        ))
        .plugin(tauri_plugin_clipboard::init())
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_global_shortcut::Builder::new().build())
        .plugin(tauri_plugin_fs::init())
        .plugin(tauri_plugin_notification::init())
        .plugin(tauri_plugin_process::init())
        .plugin(
            tauri_plugin_log::Builder::default()
                .rotation_strategy(tauri_plugin_log::RotationStrategy::KeepAll)
                .timezone_strategy(tauri_plugin_log::TimezoneStrategy::UseLocal)
                .targets(log_targets)
                .level(log_level)
                .filter(move |_| {
                    #[cfg(debug_assertions)]
                    {
                        return true;
                    }

                    #[cfg(not(debug_assertions))]
                    {
                        return enable_run_log.load(std::sync::atomic::Ordering::Relaxed);
                    }
                })
                .build(),
        )
        .setup(|app| {
            let main_window = app
                .get_webview_window("main")
                .expect("[lib::setup] no main window");

            #[cfg(target_os = "windows")]
            {
                match main_window.set_decorations(false) {
                    Ok(_) => (),
                    Err(_) => {
                        log::error!("[init_main_window] Failed to set decorations");
                    }
                }
            }

            #[cfg(target_os = "macos")]
            {
                // macOS 下不在 dock 显示
                app.set_activation_policy(tauri::ActivationPolicy::Prohibited);
            }

            // 监听窗口关闭事件，拦截关闭按钮
            let window_clone = main_window.clone();
            main_window.on_window_event(move |event| {
                if let tauri::WindowEvent::CloseRequested { api, .. } = event {
                    api.prevent_close();

                    #[cfg(target_os = "windows")]
                    {
                        if let Err(e) = window_clone.hide() {
                            log::error!("[setup] hide window error: {:?}", e);
                        }
                    }

                    #[cfg(target_os = "macos")]
                    {
                        if let Err(e) = window_clone.hide() {
                            log::error!("[setup] hide window error: {:?}", e);
                        }
                    }

                    window_clone.emit("on-hide-main-window", ()).unwrap();
                }
            });

            // 如果是调试模式，则显示窗口
            #[cfg(debug_assertions)]
            {
                main_window.show().unwrap();
            }

            Ok(())
        })
        .manage(ui_elements)
        .manage(ocr_instance)
        .manage(enigo_instance)
        .manage(scroll_screenshot_service)
        .manage(scroll_screenshot_image_service)
        .manage(scroll_screenshot_capture_service)
        .manage(video_record_service)
        .manage(free_drag_window_service)
        .manage(resize_window_service)
        .manage(listen_key_service)
        .manage(listen_mouse_service)
        .manage(file_cache_service)
        .manage(enable_run_log_clone)
        .manage(plugin_service)
        .manage(full_screen_draw_window_labels)
        .manage(webview_shared_buffer_state)
        .manage(hot_load_page_service)
        .manage(video_record_window_label)
        .manage(capture_state)
        .manage(read_clipboard_state)
        .manage(gemini_request_manager)
        .invoke_handler(tauri::generate_handler![
            screenshot::capture_current_monitor,
            screenshot::capture_all_monitors,
            screenshot::capture_focused_window,
            screenshot::get_window_elements,
            screenshot::init_ui_elements,
            screenshot::get_element_from_position,
            screenshot::init_ui_elements_cache,
            screenshot::get_mouse_position,
            screenshot::create_draw_window,
            screenshot::switch_always_on_top,
            screenshot::set_draw_window_style,
            screenshot::capture_full_screen,
            file::save_file,
            file::write_file,
            file::copy_file,
            file::remove_file,
            file::create_dir,
            file::remove_dir,
            file::get_app_config_dir,
            file::get_app_config_base_dir,
            file::create_local_config_dir,
            ocr::ocr_detect,
            #[cfg(target_os = "windows")]
            ocr::ocr_detect_with_shared_buffer,
            ocr::ocr_init,
            ocr::ocr_release,
            core::exit_app,
            core::start_free_drag,
            core::start_resize_window,
            core::close_window_after_delay,
            core::get_selected_text,
            core::set_enable_proxy,
            core::scroll_through,
            core::auto_scroll_through,
            core::click_through,
            core::create_fixed_content_window,
            core::read_image_from_clipboard,
            core::create_full_screen_draw_window,
            core::close_full_screen_draw_window,
            core::get_current_monitor_info,
            core::get_monitors_bounding_box,
            core::send_new_version_notification,
            core::create_video_record_window,
            core::close_video_record_window,
            core::has_video_record_window,
            core::has_focused_full_screen_window,
            core::set_current_window_always_on_top,
            core::auto_start_enable,
            core::auto_start_disable,
            core::restart_with_admin,
            core::write_bitmap_image_to_clipboard,
            #[cfg(target_os = "windows")]
            core::write_bitmap_image_to_clipboard_with_shared_buffer,
            core::retain_dir_files,
            core::is_admin,
            core::set_run_log,
            core::set_exclude_from_capture,
            core::show_main_window,
            core::set_window_rect,
            scroll_screenshot::scroll_screenshot_get_image_data,
            scroll_screenshot::scroll_screenshot_init,
            scroll_screenshot::scroll_screenshot_capture,
            scroll_screenshot::scroll_screenshot_handle_image,
            scroll_screenshot::scroll_screenshot_save_to_file,
            scroll_screenshot::scroll_screenshot_save_to_clipboard,
            scroll_screenshot::scroll_screenshot_get_size,
            scroll_screenshot::scroll_screenshot_clear,
            video_record::video_record_start,
            video_record::video_record_stop,
            video_record::video_record_pause,
            video_record::video_record_resume,
            video_record::video_record_kill,
            video_record::video_record_get_microphone_device_names,
            video_record::video_record_init,
            listen_key::listen_key_start,
            listen_key::listen_key_stop,
            listen_key::listen_key_stop_by_window_label,
            listen_key::listen_mouse_start,
            listen_key::listen_mouse_stop,
            listen_key::listen_mouse_stop_by_window_label,
            file::text_file_read,
            file::text_file_write,
            file::text_file_clear,
            file::is_portable_app,
            plugin::plugin_init,
            plugin::plugin_get_plugins_status,
            plugin::plugin_register_plugin,
            plugin::plugin_install_plugin,
            plugin::plugin_uninstall_plugin,
            webview::create_webview_shared_buffer,
            webview::set_support_webview_shared_buffer,
            #[cfg(target_os = "windows")]
            webview::create_webview_shared_buffer_channel,
            #[cfg(target_os = "windows")]
            core::write_image_pixels_to_clipboard_with_shared_buffer,
            http_services::upload_to_s3,
            gemini::gemini_generate_content_stream,
            gemini::gemini_cancel_stream,
            hot_load_page::hot_load_page_init,
            hot_load_page::hot_load_page_add_page,
            global_state::set_capture_state,
            global_state::get_capture_state,
            global_state::set_read_clipboard_state,
            global_state::get_read_clipboard_state,
        ])
        .on_window_event(|window, event| {
            if let tauri::WindowEvent::CloseRequested { .. } = event {
                let window_label = window.label().to_owned();

                // 用 tokio 异步进程实现清除有异步所有权问题，通知前端清理，简单处理
                match window
                    .app_handle()
                    .emit("listen-key-service:stop", window_label.clone())
                {
                    Ok(_) => (),
                    Err(e) => {
                        log::error!("[listen_key_service:stop] Failed to emit event: {}", e);
                    }
                }
                match window
                    .app_handle()
                    .emit("listen-mouse-service:stop", window_label.clone())
                {
                    Ok(_) => (),
                    Err(e) => {
                        log::error!("[listen_mouse_service:stop] Failed to emit event: {}", e);
                    }
                }
            }
        });

    #[cfg(target_os = "windows")]
    {
        app_builder = app_builder.manage(shared_buffer_service);
    }

    app_builder
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
