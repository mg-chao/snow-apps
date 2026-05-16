import { emit } from "@tauri-apps/api/event";
import {
	type Window as AppWindow,
	getCurrentWindow,
} from "@tauri-apps/api/window";
import { ConfigProvider, type ThemeConfig, theme } from "antd";
import enUS from "antd/es/locale/en_US";
import zhCN from "antd/es/locale/zh_CN";
import zhTW from "antd/es/locale/zh_TW";
import { debounce, isEqual, trim } from "es-toolkit";
import React, {
	useCallback,
	useEffect,
	useMemo,
	useRef,
	useState,
} from "react";
import { IntlProvider } from "react-intl";
import { createDrawWindow } from "@/commands";
import { createDir, textFileRead, textFileWrite } from "@/commands/file";
import { defaultAppFunctionConfigs } from "@/constants/appFunction";
import { defaultAppSettingsData } from "@/constants/appSettings";
import { defaultCommonKeyEventSettings } from "@/constants/commonKeyEvent";
import { defaultDrawToolbarKeyEventSettings } from "@/constants/drawToolbarKeyEvent";
import { PLUGIN_ID_RAPID_OCR } from "@/constants/pluginService";
import { AppContext } from "@/contexts/appContext";
import {
	AppSettingsActionContext,
	AppSettingsLoadingPublisher,
	AppSettingsPublisher,
} from "@/contexts/appSettingsActionContext";
import { usePluginServiceContext } from "@/contexts/pluginServiceContext";
import { releaseDrawPage } from "@/functions/screenshot";
import { withStatePublisher } from "@/hooks/useStatePublisher";
import { useStateSubscriber } from "@/hooks/useStateSubscriber";
import { messages } from "@/messages/map";
import {
	AppSettingsControlNode,
	type AppSettingsData,
	type AppSettingsFixedContentInitialPosition,
	AppSettingsGroup,
	AppSettingsLanguage,
	AppSettingsTheme,
	type CloudSaveUrlFormat,
	CloudSaveUrlType,
	type DoubleClickAction,
	ExtraToolList,
	type HdrColorAlgorithm,
	type HistoryValidDuration,
	OcrDetectAfterAction,
	type TrayIconClickAction,
	type TrayIconDefaultIcon,
	type VideoMaxSize,
} from "@/types/appSettings";
import type {
	AppFunction,
	AppFunctionConfig,
} from "@/types/components/appFunction";
import {
	DrawToolbarKeyEventKey,
	type DrawToolbarKeyEventValue,
} from "@/types/components/drawToolbar";
import type {
	CommonKeyEventKey,
	CommonKeyEventValue,
} from "@/types/core/commonKeyEvent";
import { DrawState } from "@/types/draw";
import { ImageFormat } from "@/types/utils/file";
import { getConfigDirPath } from "@/utils/environment";
import { appError, appWarn, formatErrorDetails } from "@/utils/log";

const getFilePath = async (group: AppSettingsGroup) => {
	const configDirPath = await getConfigDirPath();
	return `${configDirPath}/${group}.json`;
};

const AppSettingsContextProviderCore: React.FC<{
	children: React.ReactNode;
}> = ({ children }) => {
	const appWindowRef = useRef<AppWindow>(undefined);
	const [currentSystemTheme, setCurrentSystemTheme] =
		useState<AppSettingsTheme>(AppSettingsTheme.Light);
	const themeUnlisten = useRef<() => void>(() => {});
	const InitedAppContext = useRef<boolean>(false);
	const initAppContext = useCallback(async () => {
		if (InitedAppContext.current) {
			return;
		}
		InitedAppContext.current = true;

		appWindowRef.current = getCurrentWindow();
		appWindowRef.current.theme().then((theme) => {
			setCurrentSystemTheme(
				theme === "dark" ? AppSettingsTheme.Dark : AppSettingsTheme.Light,
			);
		});
		themeUnlisten.current = await appWindowRef.current.onThemeChanged(
			({ payload: theme }) => {
				setCurrentSystemTheme(
					theme === "dark" ? AppSettingsTheme.Dark : AppSettingsTheme.Light,
				);
			},
		);
	}, []);
	useEffect(() => {
		initAppContext();

		return () => {
			themeUnlisten.current();
		};
	}, [initAppContext]);

	const [appSettings, _setAppSettings] = useState<AppSettingsData>(
		defaultAppSettingsData,
	);
	const appSettingsRef = useRef<AppSettingsData>(defaultAppSettingsData);
	const [, setAppSettingsStatePublisher] = useStateSubscriber(
		AppSettingsPublisher,
		undefined,
	);
	const [, setAppSettingsLoadingPublisher] = useStateSubscriber(
		AppSettingsLoadingPublisher,
		undefined,
	);
	const setAppSettings = useCallback(
		(
			newSettings: AppSettingsData,
			ignoreState?: boolean,
			ignorePublisher?: boolean,
		) => {
			appSettingsRef.current = newSettings;
			if (!ignorePublisher) {
				setAppSettingsStatePublisher(newSettings);
			}
			if (!ignoreState) {
				_setAppSettings(newSettings);
			}
		},
		[setAppSettingsStatePublisher],
	);

	const writeAppSettings = useCallback(
		async (
			group: AppSettingsGroup,
			data: AppSettingsData[typeof group],
			syncAllWindow: boolean,
		) => {
			const filePath = await getFilePath(group);
			try {
				await textFileWrite(filePath, JSON.stringify(data));
			} catch (error) {
				appError(
					`[writeAppSettings] write file ${filePath} failed: ${JSON.stringify(error)}`,
				);
			}
			if (syncAllWindow) {
				emit("reload-app-settings");
			}
		},
		[],
	);
	const writeAppSettingsDebounce = useMemo(
		() =>
			debounce(
				(
					group: AppSettingsGroup,
					data: AppSettingsData[typeof group],
					syncAllWindow: boolean,
				) => {
					writeAppSettings(group, data, syncAllWindow);
				},
				1000,
			),
		[writeAppSettings],
	);

	const { isReady } = usePluginServiceContext();
	const updateAppSettings = useCallback(
		(
			group: AppSettingsGroup,
			val: Partial<AppSettingsData[typeof group]> | string,
			debounce: boolean,
			/** 是否保存到文件 */
			saveToFile: boolean,
			/** 是否同步到所有窗口 */
			syncAllWindow: boolean,
			/** 是否忽略状态更新 */
			ignoreState?: boolean,
			/** 是否忽略 publisher 更新 */
			ignorePublisher?: boolean,
		): AppSettingsData[typeof group] => {
			let newSettings: Partial<AppSettingsData[typeof group]>;
			if (typeof val === "string") {
				try {
					const parsedObj = JSON.parse(val);
					newSettings =
						typeof parsedObj === "object"
							? parsedObj
							: defaultAppSettingsData[group];
				} catch {
					newSettings = defaultAppSettingsData[group];
				}
			} else {
				newSettings = val;
			}

			let settings: AppSettingsData[typeof group];

			if (group === AppSettingsGroup.Common) {
				newSettings = newSettings as AppSettingsData[typeof group];
				const prevSettings = appSettingsRef.current[group] as
					| AppSettingsData[typeof group]
					| undefined;
				settings = {
					theme:
						typeof newSettings?.theme === "string"
							? newSettings.theme
							: (prevSettings?.theme ?? AppSettingsTheme.System),
					mainColor:
						typeof newSettings?.mainColor === "string"
							? newSettings.mainColor
							: (prevSettings?.mainColor ??
								defaultAppSettingsData[group].mainColor),
					borderRadius:
						typeof newSettings?.borderRadius === "number"
							? Math.min(Math.max(newSettings.borderRadius, 0), 16)
							: (prevSettings?.borderRadius ??
								defaultAppSettingsData[group].borderRadius),
					enableCompactLayout:
						typeof newSettings?.enableCompactLayout === "boolean"
							? newSettings.enableCompactLayout
							: (prevSettings?.enableCompactLayout ?? false),
					language: (() => {
						switch (newSettings?.language) {
							case "zh-Hans":
								return AppSettingsLanguage.ZHHans;
							case "zh-Hant":
								return AppSettingsLanguage.ZHHant;
							case "en":
								return AppSettingsLanguage.EN;
							default:
								return prevSettings?.language ?? AppSettingsLanguage.EN;
						}
					})(),
					browserLanguage:
						typeof newSettings?.browserLanguage === "string"
							? newSettings.browserLanguage
							: (prevSettings?.browserLanguage ?? ""),
				};

				window.__APP_ACCEPT_LANGUAGE__ = settings.language.startsWith("en")
					? "en-US"
					: "zh-CN";
			} else if (group === AppSettingsGroup.ThemeSkin) {
				newSettings = newSettings as AppSettingsData[typeof group];
				const prevSettings = appSettingsRef.current[group] as
					| AppSettingsData[typeof group]
					| undefined;
				settings = {
					skinPath:
						typeof newSettings?.skinPath === "string"
							? newSettings.skinPath
							: (prevSettings?.skinPath ??
								defaultAppSettingsData[group].skinPath),
					skinOpacity:
						typeof newSettings?.skinOpacity === "number"
							? Math.min(Math.max(newSettings.skinOpacity, 0), 100)
							: (prevSettings?.skinOpacity ??
								defaultAppSettingsData[group].skinOpacity),
					skinPosition:
						typeof newSettings?.skinPosition === "string"
							? newSettings.skinPosition
							: (prevSettings?.skinPosition ??
								defaultAppSettingsData[group].skinPosition),
					skinBlur:
						typeof newSettings?.skinBlur === "number"
							? Math.min(Math.max(newSettings.skinBlur, 0), 32)
							: (prevSettings?.skinBlur ??
								defaultAppSettingsData[group].skinBlur),
					skinImageSize:
						typeof newSettings?.skinImageSize === "string"
							? newSettings.skinImageSize
							: (prevSettings?.skinImageSize ??
								defaultAppSettingsData[group].skinImageSize),
					skinMixBlendMode:
						typeof newSettings?.skinMixBlendMode === "string"
							? newSettings.skinMixBlendMode
							: (prevSettings?.skinMixBlendMode ??
								defaultAppSettingsData[group].skinMixBlendMode),
					customCss:
						typeof newSettings?.customCss === "string"
							? newSettings.customCss
							: (prevSettings?.customCss ?? ""),
					skinMaskBlur:
						typeof newSettings?.skinMaskBlur === "number"
							? Math.min(Math.max(newSettings.skinMaskBlur, 0), 32)
							: (prevSettings?.skinMaskBlur ??
								defaultAppSettingsData[group].skinMaskBlur),
					skinMaskOpacity:
						typeof newSettings?.skinMaskOpacity === "number"
							? Math.min(Math.max(newSettings.skinMaskOpacity, 0), 100)
							: (prevSettings?.skinMaskOpacity ??
								defaultAppSettingsData[group].skinMaskOpacity),
				};
			} else if (group === AppSettingsGroup.Cache) {
				newSettings = newSettings as AppSettingsData[typeof group];
				const prevSettings = appSettingsRef.current[group] as
					| AppSettingsData[typeof group]
					| undefined;

				let prevSelectRect = newSettings?.prevSelectRect ??
					prevSettings?.prevSelectRect ?? {
						min_x: 0,
						min_y: 0,
						max_x: 0,
						max_y: 0,
					};
				prevSelectRect = {
					min_x:
						typeof prevSelectRect.min_x === "number" ? prevSelectRect.min_x : 0,
					min_y:
						typeof prevSelectRect.min_y === "number" ? prevSelectRect.min_y : 0,
					max_x:
						typeof prevSelectRect.max_x === "number" ? prevSelectRect.max_x : 0,
					max_y:
						typeof prevSelectRect.max_y === "number" ? prevSelectRect.max_y : 0,
				};

				settings = {
					menuCollapsed:
						typeof newSettings?.menuCollapsed === "boolean"
							? newSettings.menuCollapsed
							: (prevSettings?.menuCollapsed ?? false),
					chatModel:
						typeof newSettings?.chatModel === "string"
							? newSettings.chatModel
							: (prevSettings?.chatModel ??
								defaultAppSettingsData[group].chatModel),
					chatModelEnableThinking:
						typeof newSettings?.chatModelEnableThinking === "boolean"
							? newSettings.chatModelEnableThinking
							: (prevSettings?.chatModelEnableThinking ??
								defaultAppSettingsData[group].chatModelEnableThinking),
					colorPickerColorFormatIndex:
						typeof newSettings?.colorPickerColorFormatIndex === "number"
							? newSettings.colorPickerColorFormatIndex
							: (prevSettings?.colorPickerColorFormatIndex ?? 0),
					prevImageFormat:
						typeof newSettings?.prevImageFormat === "string"
							? newSettings.prevImageFormat
							: (prevSettings?.prevImageFormat ?? ImageFormat.PNG),
					prevSelectRect,
					enableMicrophone:
						typeof newSettings?.enableMicrophone === "boolean"
							? newSettings.enableMicrophone
							: (prevSettings?.enableMicrophone ?? false),
					enableLockDrawTool:
						typeof newSettings?.enableLockDrawTool === "boolean"
							? newSettings.enableLockDrawTool
							: (prevSettings?.enableLockDrawTool ?? false),
					disableArrowPicker:
						typeof newSettings?.disableArrowPicker === "boolean"
							? newSettings.disableArrowPicker
							: (prevSettings?.disableArrowPicker ?? true),
					selectRectRadius:
						typeof newSettings?.selectRectRadius === "number"
							? Math.min(Math.max(newSettings.selectRectRadius, 0), 256)
							: (prevSettings?.selectRectRadius ??
								defaultAppSettingsData[group].selectRectRadius),
					selectRectShadowWidth:
						typeof newSettings?.selectRectShadowWidth === "number"
							? Math.min(Math.max(newSettings.selectRectShadowWidth, 0), 32)
							: (prevSettings?.selectRectShadowWidth ??
								defaultAppSettingsData[group].selectRectShadowWidth),
					selectRectShadowColor:
						typeof newSettings?.selectRectShadowColor === "string"
							? newSettings.selectRectShadowColor
							: (prevSettings?.selectRectShadowColor ??
								defaultAppSettingsData[group].selectRectShadowColor),
					lastRectTool:
						typeof newSettings?.lastRectTool === "number"
							? newSettings.lastRectTool
							: (prevSettings?.lastRectTool ?? DrawState.Rect),
					lastArrowTool:
						typeof newSettings?.lastArrowTool === "number"
							? newSettings.lastArrowTool
							: (prevSettings?.lastArrowTool ?? DrawState.Arrow),
					lastFilterTool:
						typeof newSettings?.lastFilterTool === "number"
							? newSettings.lastFilterTool
							: (prevSettings?.lastFilterTool ?? DrawState.Blur),
					lastWatermarkText:
						typeof newSettings?.lastWatermarkText === "string"
							? newSettings.lastWatermarkText
							: (prevSettings?.lastWatermarkText ?? ""),
					lastExtraTool:
						typeof newSettings?.lastExtraTool === "number"
							? newSettings.lastExtraTool
							: (prevSettings?.lastExtraTool ?? ExtraToolList.None),
					lastDrawExtraTool:
						typeof newSettings?.lastDrawExtraTool === "number"
							? newSettings.lastDrawExtraTool
							: (prevSettings?.lastDrawExtraTool ?? DrawState.Idle),
					delayScreenshotSeconds:
						typeof newSettings?.delayScreenshotSeconds === "number"
							? newSettings.delayScreenshotSeconds
							: (prevSettings?.delayScreenshotSeconds ??
								defaultAppSettingsData[group].delayScreenshotSeconds),
					lockDragAspectRatio:
						typeof newSettings?.lockDragAspectRatio === "number"
							? Math.min(Math.max(newSettings.lockDragAspectRatio, 0), 100)
							: (prevSettings?.lockDragAspectRatio ??
								defaultAppSettingsData[group].lockDragAspectRatio),
					enableTabFindChildrenElements:
						typeof newSettings?.enableTabFindChildrenElements === "boolean"
							? newSettings.enableTabFindChildrenElements
							: (prevSettings?.enableTabFindChildrenElements ??
								defaultAppSettingsData[group].enableTabFindChildrenElements),
				};
			} else if (group === AppSettingsGroup.Screenshot) {
				newSettings = newSettings as AppSettingsData[typeof group];
				const prevSettings = appSettingsRef.current[group] as
					| AppSettingsData[typeof group]
					| undefined;

				let controlNode =
					prevSettings?.controlNode ?? AppSettingsControlNode.Polyline;
				if (newSettings?.controlNode) {
					if (newSettings.controlNode === AppSettingsControlNode.Circle) {
						controlNode = AppSettingsControlNode.Circle;
					} else if (
						newSettings.controlNode === AppSettingsControlNode.Polyline
					) {
						controlNode = AppSettingsControlNode.Polyline;
					}
				}

				settings = {
					uiScale:
						typeof newSettings?.uiScale === "number"
							? Math.min(Math.max(newSettings.uiScale, 25), 100)
							: (prevSettings?.uiScale ??
								defaultAppSettingsData[group].uiScale),
					toolbarUiScale:
						typeof newSettings?.toolbarUiScale === "number"
							? Math.min(Math.max(newSettings.toolbarUiScale, 25), 100)
							: (prevSettings?.toolbarUiScale ??
								defaultAppSettingsData[group].toolbarUiScale),
					controlNode,
					disableAnimation:
						typeof newSettings?.disableAnimation === "boolean"
							? newSettings.disableAnimation
							: (prevSettings?.disableAnimation ?? false),
					colorPickerShowMode:
						typeof newSettings?.colorPickerShowMode === "number"
							? newSettings.colorPickerShowMode
							: (prevSettings?.colorPickerShowMode ??
								defaultAppSettingsData[group].colorPickerShowMode),
					beyondSelectRectElementOpacity:
						typeof newSettings?.beyondSelectRectElementOpacity === "number"
							? Math.min(
									Math.max(newSettings.beyondSelectRectElementOpacity, 0),
									100,
								)
							: (prevSettings?.beyondSelectRectElementOpacity ??
								defaultAppSettingsData[group].beyondSelectRectElementOpacity),
					selectRectMaskColor:
						typeof newSettings?.selectRectMaskColor === "string"
							? newSettings.selectRectMaskColor
							: (prevSettings?.selectRectMaskColor ??
								defaultAppSettingsData[group].selectRectMaskColor),
					hotKeyTipOpacity:
						typeof newSettings?.hotKeyTipOpacity === "number"
							? Math.min(Math.max(newSettings.hotKeyTipOpacity, 0), 100)
							: (prevSettings?.hotKeyTipOpacity ??
								defaultAppSettingsData[group].hotKeyTipOpacity),
					fullScreenAuxiliaryLineColor:
						typeof newSettings?.fullScreenAuxiliaryLineColor === "string"
							? newSettings.fullScreenAuxiliaryLineColor
							: (prevSettings?.fullScreenAuxiliaryLineColor ?? "#00000000"),
					monitorCenterAuxiliaryLineColor:
						typeof newSettings?.monitorCenterAuxiliaryLineColor === "string"
							? newSettings.monitorCenterAuxiliaryLineColor
							: (prevSettings?.monitorCenterAuxiliaryLineColor ?? "#00000000"),
					colorPickerCenterAuxiliaryLineColor:
						typeof newSettings?.colorPickerCenterAuxiliaryLineColor === "string"
							? newSettings.colorPickerCenterAuxiliaryLineColor
							: (prevSettings?.colorPickerCenterAuxiliaryLineColor ??
								"#00000000"),
					toolbarHiddenToolList:
						typeof newSettings?.toolbarHiddenToolList === "object"
							? newSettings.toolbarHiddenToolList
							: (prevSettings?.toolbarHiddenToolList ??
								defaultAppSettingsData[group].toolbarHiddenToolList),
				};
			} else if (group === AppSettingsGroup.FunctionDraw) {
				newSettings = newSettings as AppSettingsData[typeof group];
				const prevSettings = appSettingsRef.current[group] as
					| AppSettingsData[typeof group]
					| undefined;

				settings = {
					lockDrawTool:
						typeof newSettings?.lockDrawTool === "boolean"
							? newSettings.lockDrawTool
							: (prevSettings?.lockDrawTool ??
								defaultAppSettingsData[group].lockDrawTool),
					disableQuickSelectElementToolList: Array.isArray(
						newSettings?.disableQuickSelectElementToolList,
					)
						? newSettings.disableQuickSelectElementToolList
						: (prevSettings?.disableQuickSelectElementToolList ??
							defaultAppSettingsData[group].disableQuickSelectElementToolList),
					enableSliderChangeWidth:
						typeof newSettings?.enableSliderChangeWidth === "boolean"
							? newSettings.enableSliderChangeWidth
							: (prevSettings?.enableSliderChangeWidth ??
								defaultAppSettingsData[group].enableSliderChangeWidth),
					toolIndependentStyle:
						typeof newSettings?.toolIndependentStyle === "boolean"
							? newSettings.toolIndependentStyle
							: (prevSettings?.toolIndependentStyle ??
								defaultAppSettingsData[group].toolIndependentStyle),
				};
			} else if (group === AppSettingsGroup.FixedContent) {
				newSettings = newSettings as AppSettingsData[typeof group];
				const prevSettings = appSettingsRef.current[group] as
					| AppSettingsData[typeof group]
					| undefined;

				settings = {
					borderColor:
						typeof newSettings?.borderColor === "string"
							? newSettings.borderColor
							: (prevSettings?.borderColor ??
								defaultAppSettingsData[group].borderColor),
				};
			} else if (group === AppSettingsGroup.DrawToolbarKeyEvent) {
				newSettings = newSettings as AppSettingsData[typeof group];
				const prevSettings = appSettingsRef.current[group] as
					| AppSettingsData[typeof group]
					| undefined;

				const settingsKeySet = new Set<string>();
				const settingKeys: DrawToolbarKeyEventKey[] = Object.keys(
					defaultDrawToolbarKeyEventSettings,
				).filter((key) => {
					if (
						key === DrawToolbarKeyEventKey.OcrDetectTool ||
						key === DrawToolbarKeyEventKey.OcrTranslateTool
					) {
						return isReady?.(PLUGIN_ID_RAPID_OCR);
					}

					return true;
				}) as DrawToolbarKeyEventKey[];
				settingKeys.forEach((key) => {
					const keyEventSettings = newSettings as Record<
						DrawToolbarKeyEventKey,
						DrawToolbarKeyEventValue
					>;

					let keyEventSettingsKey =
						typeof keyEventSettings[key]?.hotKey === "string"
							? keyEventSettings[key].hotKey
							: (prevSettings?.[key]?.hotKey ??
								defaultDrawToolbarKeyEventSettings[key].hotKey);

					// 格式化处理下
					keyEventSettingsKey = keyEventSettingsKey
						.split(",")
						.map((item) => trim(item))
						.filter((val) => {
							if (settingsKeySet.has(val)) {
								return false;
							}

							if (defaultDrawToolbarKeyEventSettings[key].unique) {
								settingsKeySet.add(val);
							}

							return true;
						})
						.join(", ");

					keyEventSettings[key] = {
						hotKey: keyEventSettingsKey,
					};
				});

				settings = {
					...defaultDrawToolbarKeyEventSettings,
					...newSettings,
				};
			} else if (group === AppSettingsGroup.CommonKeyEvent) {
				newSettings = newSettings as AppSettingsData[typeof group];
				const prevSettings = appSettingsRef.current[group] as
					| AppSettingsData[typeof group]
					| undefined;

				const settingsKeySet = new Set<string>();
				const settingKeys: CommonKeyEventKey[] = Object.keys(
					defaultCommonKeyEventSettings,
				) as CommonKeyEventKey[];
				settingKeys.forEach((key) => {
					const keyEventSettings = newSettings as Record<
						CommonKeyEventKey,
						CommonKeyEventValue
					>;

					let keyEventSettingsKey =
						typeof keyEventSettings[key]?.hotKey === "string"
							? keyEventSettings[key].hotKey
							: (prevSettings?.[key]?.hotKey ??
								defaultCommonKeyEventSettings[key].hotKey);

					// 格式化处理下
					keyEventSettingsKey = keyEventSettingsKey
						.split(",")
						.map((item) => trim(item))
						.filter((val) => {
							if (settingsKeySet.has(val)) {
								return false;
							}

							if (defaultCommonKeyEventSettings[key].unique) {
								settingsKeySet.add(val);
							}

							return true;
						})
						.join(", ");

					keyEventSettings[key] = {
						hotKey: keyEventSettingsKey,
						group: defaultCommonKeyEventSettings[key].group,
					};
				});

				settings = {
					...defaultCommonKeyEventSettings,
					...newSettings,
				};
			} else if (group === AppSettingsGroup.AppFunction) {
				newSettings = newSettings as AppSettingsData[typeof group];
				const prevSettings = appSettingsRef.current[group] as
					| AppSettingsData[typeof group]
					| undefined;

				const settingsKeySet = new Set<string>();
				const settingKeys: AppFunction[] = Object.keys(
					defaultAppFunctionConfigs,
				) as AppFunction[];
				settingKeys.forEach((key) => {
					const keyEventSettings = newSettings as Record<
						AppFunction,
						AppFunctionConfig
					>;

					let keyEventSettingsKey =
						typeof keyEventSettings[key]?.shortcutKey === "string"
							? keyEventSettings[key].shortcutKey
							: (prevSettings?.[key]?.shortcutKey ??
								defaultAppFunctionConfigs[key].shortcutKey);

					// 格式化处理下
					keyEventSettingsKey = keyEventSettingsKey
						.split(",")
						.slice(0, 1) // 快捷键不支持多个键，这里也限制下
						.map((item) => trim(item))
						.filter((val) => {
							if (settingsKeySet.has(val)) {
								return false;
							}

							return true;
						})
						.join(", ");

					settingsKeySet.add(keyEventSettingsKey);

					keyEventSettings[key] = {
						shortcutKey: keyEventSettingsKey,
						group: defaultAppFunctionConfigs[key].group,
					};
				});

				settings = {
					...defaultAppFunctionConfigs,
					...newSettings,
				};
			} else if (group === AppSettingsGroup.Render) {
				newSettings = newSettings as AppSettingsData[typeof group];
				const prevSettings = appSettingsRef.current[group] as
					| AppSettingsData[typeof group]
					| undefined;

				settings = {
					antialias:
						typeof newSettings?.antialias === "boolean"
							? newSettings.antialias
							: (prevSettings?.antialias ??
								defaultAppSettingsData[group].antialias),
				};
			} else if (group === AppSettingsGroup.SystemCommon) {
				newSettings = newSettings as AppSettingsData[typeof group];
				const prevSettings = appSettingsRef.current[group] as
					| AppSettingsData[typeof group]
					| undefined;

				settings = {
					autoStart:
						typeof newSettings?.autoStart === "boolean"
							? newSettings.autoStart
							: (prevSettings?.autoStart ??
								defaultAppSettingsData[group].autoStart),
					autoCheckVersion:
						typeof newSettings?.autoCheckVersion === "boolean"
							? newSettings.autoCheckVersion
							: (prevSettings?.autoCheckVersion ??
								defaultAppSettingsData[group].autoCheckVersion),
					runLog:
						typeof newSettings?.runLog === "boolean"
							? newSettings.runLog
							: (prevSettings?.runLog ?? defaultAppSettingsData[group].runLog),
				};
			} else if (group === AppSettingsGroup.SystemChat) {
				newSettings = newSettings as AppSettingsData[typeof group];
				const prevSettings = appSettingsRef.current[group] as
					| AppSettingsData[typeof group]
					| undefined;

				settings = {
					maxTokens:
						typeof newSettings?.maxTokens === "number"
							? Math.min(Math.max(newSettings.maxTokens, 512), 8192)
							: (prevSettings?.maxTokens ??
								defaultAppSettingsData[group].maxTokens),
					temperature:
						typeof newSettings?.temperature === "number"
							? Math.min(Math.max(newSettings.temperature, 0), 2)
							: (prevSettings?.temperature ??
								defaultAppSettingsData[group].temperature),
					thinkingBudgetTokens:
						typeof newSettings?.thinkingBudgetTokens === "number"
							? Math.min(Math.max(newSettings.thinkingBudgetTokens, 1024), 8192)
							: (prevSettings?.thinkingBudgetTokens ??
								defaultAppSettingsData[group].thinkingBudgetTokens),
				};
			} else if (group === AppSettingsGroup.SystemNetwork) {
				newSettings = newSettings as AppSettingsData[typeof group];
				const prevSettings = appSettingsRef.current[group] as
					| AppSettingsData[typeof group]
					| undefined;

				settings = {
					enableProxy:
						typeof newSettings?.enableProxy === "boolean"
							? newSettings.enableProxy
							: (prevSettings?.enableProxy ??
								defaultAppSettingsData[group].enableProxy),
				};
			} else if (group === AppSettingsGroup.FunctionOcr) {
				newSettings = newSettings as AppSettingsData[typeof group];
				const prevSettings = appSettingsRef.current[group] as
					| AppSettingsData[typeof group]
					| undefined;

				settings = {
					ocrModel:
						typeof newSettings?.ocrModel === "string"
							? newSettings.ocrModel
							: (prevSettings?.ocrModel ??
								defaultAppSettingsData[group].ocrModel),
					htmlVisionModel:
						typeof newSettings?.htmlVisionModel === "string"
							? newSettings.htmlVisionModel
							: (prevSettings?.htmlVisionModel ??
								defaultAppSettingsData[group].htmlVisionModel),
					htmlVisionModelSystemPrompt:
						typeof newSettings?.htmlVisionModelSystemPrompt === "string"
							? newSettings.htmlVisionModelSystemPrompt
							: (prevSettings?.htmlVisionModelSystemPrompt ??
								defaultAppSettingsData[group].htmlVisionModelSystemPrompt),
					markdownVisionModelSystemPrompt:
						typeof newSettings?.markdownVisionModelSystemPrompt === "string"
							? newSettings.markdownVisionModelSystemPrompt
							: (prevSettings?.markdownVisionModelSystemPrompt ??
								defaultAppSettingsData[group].markdownVisionModelSystemPrompt),
				};
			} else if (group === AppSettingsGroup.FunctionChat) {
				newSettings = newSettings as AppSettingsData[typeof group];
				const prevSettings = appSettingsRef.current[group] as
					| AppSettingsData[typeof group]
					| undefined;

				settings = {
					autoCreateNewSession:
						typeof newSettings?.autoCreateNewSession === "boolean"
							? newSettings.autoCreateNewSession
							: (prevSettings?.autoCreateNewSession ??
								defaultAppSettingsData[group].autoCreateNewSession),
					chatApiConfigList: Array.isArray(newSettings?.chatApiConfigList)
						? newSettings.chatApiConfigList.map((item) => ({
								api_uri: `${item.api_uri ?? ""}`,
								api_key: `${item.api_key ?? ""}`,
								api_model: `${item.api_model ?? ""}`,
								model_name: `${item.model_name ?? ""}`,
								support_thinking: !!item.support_thinking,
								support_vision: !!item.support_vision,
							}))
						: (prevSettings?.chatApiConfigList ??
							defaultAppSettingsData[group].chatApiConfigList),
					autoCreateNewSessionOnCloseWindow:
						typeof newSettings?.autoCreateNewSessionOnCloseWindow === "boolean"
							? newSettings.autoCreateNewSessionOnCloseWindow
							: (prevSettings?.autoCreateNewSessionOnCloseWindow ??
								defaultAppSettingsData[group]
									.autoCreateNewSessionOnCloseWindow),
				};
			} else if (group === AppSettingsGroup.FunctionTranslationCache) {
				newSettings = newSettings as AppSettingsData[typeof group];
				const prevSettings = appSettingsRef.current[group] as
					| AppSettingsData[typeof group]
					| undefined;

				settings = {
					cacheSourceLanguage:
						typeof newSettings?.cacheSourceLanguage === "string"
							? newSettings.cacheSourceLanguage
							: (prevSettings?.cacheSourceLanguage ??
								defaultAppSettingsData[group].cacheSourceLanguage),
					cacheTargetLanguage:
						typeof newSettings?.cacheTargetLanguage === "string"
							? newSettings.cacheTargetLanguage
							: (prevSettings?.cacheTargetLanguage ??
								defaultAppSettingsData[group].cacheTargetLanguage),
					cacheTranslationDomain:
						typeof newSettings?.cacheTranslationDomain === "string"
							? newSettings.cacheTranslationDomain
							: (prevSettings?.cacheTranslationDomain ??
								defaultAppSettingsData[group].cacheTranslationDomain),
					cacheTranslationType:
						typeof newSettings?.cacheTranslationType === "number" ||
						typeof newSettings?.cacheTranslationType === "string"
							? newSettings.cacheTranslationType
							: (prevSettings?.cacheTranslationType ??
								defaultAppSettingsData[group].cacheTranslationType),
				};
			} else if (group === AppSettingsGroup.FunctionTranslation) {
				newSettings = newSettings as AppSettingsData[typeof group];
				const prevSettings = appSettingsRef.current[group] as
					| AppSettingsData[typeof group]
					| undefined;

				settings = {
					translationSystemPrompt:
						typeof newSettings?.translationSystemPrompt === "string"
							? newSettings.translationSystemPrompt
							: (prevSettings?.translationSystemPrompt ??
								defaultAppSettingsData[group].translationSystemPrompt),
					optimizeAiTranslationLayout:
						typeof newSettings?.optimizeAiTranslationLayout === "boolean"
							? newSettings.optimizeAiTranslationLayout
							: (prevSettings?.optimizeAiTranslationLayout ??
								defaultAppSettingsData[group].optimizeAiTranslationLayout),
					translationApiConfigList: Array.isArray(
						newSettings?.translationApiConfigList,
					)
						? newSettings.translationApiConfigList.map((item) => ({
								api_uri: `${item.api_uri ?? ""}`,
								api_key: `${item.api_key ?? ""}`,
								api_type: item.api_type,
								deepl_prefer_quality_optimized:
									typeof item.deepl_prefer_quality_optimized === "boolean"
										? item.deepl_prefer_quality_optimized
										: false,
							}))
						: (prevSettings?.translationApiConfigList ??
							defaultAppSettingsData[group].translationApiConfigList),
					sourceLanguage:
						typeof newSettings?.sourceLanguage === "string"
							? newSettings.sourceLanguage
							: (prevSettings?.sourceLanguage ??
								defaultAppSettingsData[group].sourceLanguage),
					targetLanguage:
						typeof newSettings?.targetLanguage === "string"
							? newSettings.targetLanguage
							: (prevSettings?.targetLanguage ??
								defaultAppSettingsData[group].targetLanguage),
					translationDomain:
						typeof newSettings?.translationDomain === "string"
							? newSettings.translationDomain
							: (prevSettings?.translationDomain ??
								defaultAppSettingsData[group].translationDomain),
					translationType:
						typeof newSettings?.translationType === "number" ||
						typeof newSettings?.translationType === "string"
							? newSettings.translationType
							: (prevSettings?.translationType ??
								defaultAppSettingsData[group].translationType),
				};
			} else if (group === AppSettingsGroup.FunctionScreenshot) {
				newSettings = newSettings as AppSettingsData[typeof group];
				const prevSettings = appSettingsRef.current[group] as
					| AppSettingsData[typeof group]
					| undefined;

				const findChildrenElements =
					typeof newSettings?.findChildrenElements === "boolean"
						? newSettings.findChildrenElements
						: (prevSettings?.findChildrenElements ??
							defaultAppSettingsData[group].findChildrenElements);

				const dragSelectRectFromOutside =
					typeof newSettings?.dragSelectRectFromOutside === "boolean"
						? newSettings.dragSelectRectFromOutside
						: (prevSettings?.dragSelectRectFromOutside ??
							defaultAppSettingsData[group].dragSelectRectFromOutside);

				settings = {
					findChildrenElements,
					dragSelectRectFromOutside,
					shortcutCanleTip:
						typeof newSettings?.shortcutCanleTip === "boolean"
							? newSettings.shortcutCanleTip
							: (prevSettings?.shortcutCanleTip ??
								defaultAppSettingsData[group].shortcutCanleTip),
					cloudSaveUrlFormat:
						typeof newSettings?.cloudSaveUrlFormat === "string"
							? (newSettings.cloudSaveUrlFormat as CloudSaveUrlFormat)
							: (prevSettings?.cloudSaveUrlFormat ??
								defaultAppSettingsData[group].cloudSaveUrlFormat),
					cloudProxyUrl:
						typeof newSettings?.cloudProxyUrl === "string"
							? newSettings.cloudProxyUrl
							: (prevSettings?.cloudProxyUrl ??
								defaultAppSettingsData[group].cloudProxyUrl),
					autoSaveOnCopy:
						typeof newSettings?.autoSaveOnCopy === "boolean"
							? newSettings.autoSaveOnCopy
							: (prevSettings?.autoSaveOnCopy ?? false),
					fastSave:
						typeof newSettings?.fastSave === "boolean"
							? newSettings.fastSave
							: (prevSettings?.fastSave ?? false),
					doubleClickAction:
						typeof newSettings?.doubleClickAction === "string"
							? (newSettings.doubleClickAction as DoubleClickAction)
							: (prevSettings?.doubleClickAction ??
								defaultAppSettingsData[group].doubleClickAction),
					copyImageFileToClipboard:
						typeof newSettings?.copyImageFileToClipboard === "boolean"
							? newSettings.copyImageFileToClipboard
							: (prevSettings?.copyImageFileToClipboard ??
								defaultAppSettingsData[group].copyImageFileToClipboard),
					saveToCloud:
						typeof newSettings?.saveToCloud === "boolean"
							? newSettings.saveToCloud
							: (prevSettings?.saveToCloud ?? false),
					cloudSaveUrlType:
						typeof newSettings?.cloudSaveUrlType === "string"
							? (newSettings.cloudSaveUrlType as CloudSaveUrlType)
							: (prevSettings?.cloudSaveUrlType ?? CloudSaveUrlType.S3),
					s3AccessKeyId:
						typeof newSettings?.s3AccessKeyId === "string"
							? newSettings.s3AccessKeyId
							: (prevSettings?.s3AccessKeyId ??
								defaultAppSettingsData[group].s3AccessKeyId),
					s3SecretAccessKey:
						typeof newSettings?.s3SecretAccessKey === "string"
							? newSettings.s3SecretAccessKey
							: (prevSettings?.s3SecretAccessKey ??
								defaultAppSettingsData[group].s3SecretAccessKey),
					s3Region:
						typeof newSettings?.s3Region === "string"
							? newSettings.s3Region
							: (prevSettings?.s3Region ??
								defaultAppSettingsData[group].s3Region),
					s3Endpoint:
						typeof newSettings?.s3Endpoint === "string"
							? newSettings.s3Endpoint
							: (prevSettings?.s3Endpoint ??
								defaultAppSettingsData[group].s3Endpoint),
					s3BucketName:
						typeof newSettings?.s3BucketName === "string"
							? newSettings.s3BucketName
							: (prevSettings?.s3BucketName ??
								defaultAppSettingsData[group].s3BucketName),
					s3PathPrefix:
						typeof newSettings?.s3PathPrefix === "string"
							? newSettings.s3PathPrefix
							: (prevSettings?.s3PathPrefix ??
								defaultAppSettingsData[group].s3PathPrefix),
					s3ForcePathStyle:
						typeof newSettings?.s3ForcePathStyle === "boolean"
							? newSettings.s3ForcePathStyle
							: (prevSettings?.s3ForcePathStyle ?? false),
					saveFileDirectory:
						typeof newSettings?.saveFileDirectory === "string"
							? newSettings.saveFileDirectory
							: (prevSettings?.saveFileDirectory ??
								defaultAppSettingsData[group].saveFileDirectory),
					saveFileFormat:
						typeof newSettings?.saveFileFormat === "string"
							? newSettings.saveFileFormat
							: (prevSettings?.saveFileFormat ?? ImageFormat.PNG),
					ocrAfterAction:
						typeof newSettings?.ocrAfterAction === "string"
							? (newSettings.ocrAfterAction as OcrDetectAfterAction)
							: (prevSettings?.ocrAfterAction ?? OcrDetectAfterAction.None),
					ocrCopyText:
						typeof newSettings?.ocrCopyText === "boolean"
							? newSettings.ocrCopyText
							: (prevSettings?.ocrCopyText ?? false),
					focusedWindowCopyToClipboard:
						typeof newSettings?.focusedWindowCopyToClipboard === "boolean"
							? newSettings.focusedWindowCopyToClipboard
							: (prevSettings?.focusedWindowCopyToClipboard ?? true),
					fullScreenCopyToClipboard:
						typeof newSettings?.fullScreenCopyToClipboard === "boolean"
							? newSettings.fullScreenCopyToClipboard
							: (prevSettings?.fullScreenCopyToClipboard ?? true),
					selectRectPresetList:
						typeof newSettings?.selectRectPresetList === "object"
							? newSettings.selectRectPresetList
							: (prevSettings?.selectRectPresetList ?? []),
				};
			} else if (group === AppSettingsGroup.FunctionOutput) {
				newSettings = newSettings as AppSettingsData[typeof group];
				const prevSettings = appSettingsRef.current[group] as
					| AppSettingsData[typeof group]
					| undefined;

				settings = {
					manualSaveFileNameFormat:
						typeof newSettings?.manualSaveFileNameFormat === "string"
							? newSettings.manualSaveFileNameFormat
							: (prevSettings?.manualSaveFileNameFormat ??
								defaultAppSettingsData[group].manualSaveFileNameFormat),
					autoSaveFileNameFormat:
						typeof newSettings?.autoSaveFileNameFormat === "string"
							? newSettings.autoSaveFileNameFormat
							: (prevSettings?.autoSaveFileNameFormat ??
								defaultAppSettingsData[group].autoSaveFileNameFormat),
					fastSaveFileNameFormat:
						typeof newSettings?.fastSaveFileNameFormat === "string"
							? newSettings.fastSaveFileNameFormat
							: (prevSettings?.fastSaveFileNameFormat ??
								defaultAppSettingsData[group].fastSaveFileNameFormat),
					focusedWindowFileNameFormat:
						typeof newSettings?.focusedWindowFileNameFormat === "string"
							? newSettings.focusedWindowFileNameFormat
							: (prevSettings?.focusedWindowFileNameFormat ??
								defaultAppSettingsData[group].focusedWindowFileNameFormat),
					fullScreenFileNameFormat:
						typeof newSettings?.fullScreenFileNameFormat === "string"
							? newSettings.fullScreenFileNameFormat
							: (prevSettings?.fullScreenFileNameFormat ??
								defaultAppSettingsData[group].fullScreenFileNameFormat),
					videoRecordFileNameFormat:
						typeof newSettings?.videoRecordFileNameFormat === "string"
							? newSettings.videoRecordFileNameFormat
							: (prevSettings?.videoRecordFileNameFormat ??
								defaultAppSettingsData[group].videoRecordFileNameFormat),
					uploadToCloudSaveUrlFormat:
						typeof newSettings?.uploadToCloudSaveUrlFormat === "string"
							? newSettings.uploadToCloudSaveUrlFormat
							: (prevSettings?.uploadToCloudSaveUrlFormat ??
								defaultAppSettingsData[group].uploadToCloudSaveUrlFormat),
				};
			} else if (group === AppSettingsGroup.FunctionFullScreenDraw) {
				newSettings = newSettings as AppSettingsData[typeof group];
				const prevSettings = appSettingsRef.current[group] as
					| AppSettingsData[typeof group]
					| undefined;

				settings = {
					defaultTool:
						typeof newSettings?.defaultTool === "number"
							? newSettings.defaultTool
							: (prevSettings?.defaultTool ?? DrawState.Select),
				};
			} else if (group === AppSettingsGroup.SystemScrollScreenshot) {
				newSettings = newSettings as AppSettingsData[typeof group];
				const prevSettings = appSettingsRef.current[group] as
					| AppSettingsData[typeof group]
					| undefined;

				settings = {
					imageFeatureThreshold:
						typeof newSettings?.imageFeatureThreshold === "number"
							? Math.min(Math.max(newSettings.imageFeatureThreshold, 0), 255)
							: (prevSettings?.imageFeatureThreshold ??
								defaultAppSettingsData[group].imageFeatureThreshold),
					minSide:
						typeof newSettings?.minSide === "number"
							? Math.min(Math.max(newSettings.minSide, 0), 1024)
							: (prevSettings?.minSide ??
								defaultAppSettingsData[group].minSide),
					maxSide:
						typeof newSettings?.maxSide === "number"
							? Math.min(Math.max(newSettings.maxSide, 64), 1024)
							: (prevSettings?.maxSide ??
								defaultAppSettingsData[group].maxSide),
					sampleRate:
						typeof newSettings?.sampleRate === "number"
							? Math.min(Math.max(newSettings.sampleRate, 0.1), 1)
							: (prevSettings?.sampleRate ??
								defaultAppSettingsData[group].sampleRate),
					imageFeatureDescriptionLength:
						typeof newSettings?.imageFeatureDescriptionLength === "number"
							? Math.min(
									Math.max(newSettings.imageFeatureDescriptionLength, 8),
									128,
								)
							: (prevSettings?.imageFeatureDescriptionLength ??
								defaultAppSettingsData[group].imageFeatureDescriptionLength),
					tryRollback:
						typeof newSettings?.tryRollback === "boolean"
							? newSettings.tryRollback
							: (prevSettings?.tryRollback ??
								defaultAppSettingsData[group].tryRollback),
				};
			} else if (group === AppSettingsGroup.FunctionTrayIcon) {
				newSettings = newSettings as AppSettingsData[typeof group];
				const prevSettings = appSettingsRef.current[group] as
					| AppSettingsData[typeof group]
					| undefined;

				settings = {
					iconClickAction:
						typeof newSettings?.iconClickAction === "string"
							? (newSettings.iconClickAction as TrayIconClickAction)
							: (prevSettings?.iconClickAction ??
								defaultAppSettingsData[group].iconClickAction),
				};
			} else if (group === AppSettingsGroup.CommonTrayIcon) {
				newSettings = newSettings as AppSettingsData[typeof group];
				const prevSettings = appSettingsRef.current[group] as
					| AppSettingsData[typeof group]
					| undefined;

				settings = {
					iconPath:
						typeof newSettings?.iconPath === "string"
							? newSettings.iconPath
							: (prevSettings?.iconPath ?? ""),
					defaultIcons:
						typeof newSettings?.defaultIcons === "string"
							? (newSettings.defaultIcons as TrayIconDefaultIcon)
							: (prevSettings?.defaultIcons ??
								defaultAppSettingsData[group].defaultIcons),
					enableTrayIcon:
						typeof newSettings?.enableTrayIcon === "boolean"
							? newSettings.enableTrayIcon
							: (prevSettings?.enableTrayIcon ??
								defaultAppSettingsData[group].enableTrayIcon),
					iconPathDark:
						typeof newSettings?.iconPathDark === "string"
							? newSettings.iconPathDark
							: (prevSettings?.iconPathDark ?? ""),
					defaultIconsDark:
						typeof newSettings?.defaultIconsDark === "string"
							? (newSettings.defaultIconsDark as TrayIconDefaultIcon)
							: (prevSettings?.defaultIconsDark ??
								defaultAppSettingsData[group].defaultIcons),
				};
			} else if (group === AppSettingsGroup.FunctionVideoRecord) {
				newSettings = newSettings as AppSettingsData[typeof group];
				const prevSettings = appSettingsRef.current[group] as
					| AppSettingsData[typeof group]
					| undefined;

				settings = {
					enableExcludeFromCapture:
						typeof newSettings?.enableExcludeFromCapture === "boolean"
							? newSettings.enableExcludeFromCapture
							: (prevSettings?.enableExcludeFromCapture ??
								defaultAppSettingsData[group].enableExcludeFromCapture),
					saveDirectory:
						typeof newSettings?.saveDirectory === "string"
							? newSettings.saveDirectory
							: (prevSettings?.saveDirectory ?? ""),
					frameRate:
						typeof newSettings?.frameRate === "number"
							? Math.min(Math.max(newSettings.frameRate, 1), 120)
							: (prevSettings?.frameRate ??
								defaultAppSettingsData[group].frameRate),
					microphoneDeviceName:
						typeof newSettings?.microphoneDeviceName === "string"
							? newSettings.microphoneDeviceName
							: (prevSettings?.microphoneDeviceName ?? ""),
					hwaccel:
						typeof newSettings?.hwaccel === "boolean"
							? newSettings.hwaccel
							: (prevSettings?.hwaccel ??
								defaultAppSettingsData[group].hwaccel),
					encoder:
						typeof newSettings?.encoder === "string"
							? newSettings.encoder
							: (prevSettings?.encoder ??
								defaultAppSettingsData[group].encoder),
					encoderPreset:
						typeof newSettings?.encoderPreset === "string"
							? newSettings.encoderPreset
							: (prevSettings?.encoderPreset ??
								defaultAppSettingsData[group].encoderPreset),
					videoMaxSize:
						typeof newSettings?.videoMaxSize === "string"
							? (newSettings.videoMaxSize as VideoMaxSize)
							: (prevSettings?.videoMaxSize ??
								defaultAppSettingsData[group].videoMaxSize),
					gifFrameRate:
						typeof newSettings?.gifFrameRate === "number"
							? Math.min(Math.max(newSettings.gifFrameRate, 1), 24)
							: (prevSettings?.gifFrameRate ??
								defaultAppSettingsData[group].gifFrameRate),
					gifMaxSize:
						typeof newSettings?.gifMaxSize === "string"
							? (newSettings.gifMaxSize as VideoMaxSize)
							: (prevSettings?.gifMaxSize ??
								defaultAppSettingsData[group].gifMaxSize),
					gifFormat:
						typeof newSettings?.gifFormat === "string"
							? newSettings.gifFormat
							: (prevSettings?.gifFormat ??
								defaultAppSettingsData[group].gifFormat),
					keyDisplayFontSize:
						typeof newSettings?.keyDisplayFontSize === "number"
							? Math.min(Math.max(newSettings.keyDisplayFontSize, 8), 64)
							: (prevSettings?.keyDisplayFontSize ??
								defaultAppSettingsData[group].keyDisplayFontSize),
					keyDisplayBackgroundColor:
						typeof newSettings?.keyDisplayBackgroundColor === "string"
							? trim(newSettings.keyDisplayBackgroundColor)
							: (prevSettings?.keyDisplayBackgroundColor ??
								defaultAppSettingsData[group].keyDisplayBackgroundColor),
					keyDisplayTextColor:
						typeof newSettings?.keyDisplayTextColor === "string"
							? trim(newSettings.keyDisplayTextColor)
							: (prevSettings?.keyDisplayTextColor ??
								defaultAppSettingsData[group].keyDisplayTextColor),
					keyDisplayDuration:
						typeof newSettings?.keyDisplayDuration === "number"
							? Math.min(Math.max(newSettings.keyDisplayDuration, 100), 10000)
							: (prevSettings?.keyDisplayDuration ??
								defaultAppSettingsData[group].keyDisplayDuration),
					keyDisplayMergeDuration:
						typeof newSettings?.keyDisplayMergeDuration === "number"
							? Math.min(Math.max(newSettings.keyDisplayMergeDuration, 0), 2000)
							: (prevSettings?.keyDisplayMergeDuration ??
								defaultAppSettingsData[group].keyDisplayMergeDuration),
					keyDisplayDirection:
						typeof newSettings?.keyDisplayDirection === "string"
							? newSettings.keyDisplayDirection
							: (prevSettings?.keyDisplayDirection ??
								defaultAppSettingsData[group].keyDisplayDirection),
				};
			} else if (group === AppSettingsGroup.FunctionFixedContent) {
				newSettings = newSettings as AppSettingsData[typeof group];
				const prevSettings = appSettingsRef.current[group] as
					| AppSettingsData[typeof group]
					| undefined;

				settings = {
					zoomWithMouse:
						typeof newSettings?.zoomWithMouse === "boolean"
							? newSettings.zoomWithMouse
							: (prevSettings?.zoomWithMouse ?? false),
					autoResizeWindow:
						typeof newSettings?.autoResizeWindow === "boolean"
							? newSettings.autoResizeWindow
							: (prevSettings?.autoResizeWindow ??
								defaultAppSettingsData[group].autoResizeWindow),
					autoOcr:
						typeof newSettings?.autoOcr === "boolean"
							? newSettings.autoOcr
							: (prevSettings?.autoOcr ??
								defaultAppSettingsData[group].autoOcr),
					autoCopyToClipboard:
						typeof newSettings?.autoCopyToClipboard === "boolean"
							? newSettings.autoCopyToClipboard
							: (prevSettings?.autoCopyToClipboard ??
								defaultAppSettingsData[group].autoCopyToClipboard),
					initialPosition:
						typeof newSettings?.initialPosition === "string"
							? (newSettings.initialPosition as AppSettingsFixedContentInitialPosition)
							: (prevSettings?.initialPosition ??
								defaultAppSettingsData[group].initialPosition),
				};
			} else if (group === AppSettingsGroup.SystemScreenshot) {
				newSettings = newSettings as AppSettingsData[typeof group];
				const prevSettings = appSettingsRef.current[group] as
					| AppSettingsData[typeof group]
					| undefined;

				settings = {
					ocrHotStart:
						typeof newSettings?.ocrHotStart === "boolean"
							? newSettings.ocrHotStart
							: (prevSettings?.ocrHotStart ??
								defaultAppSettingsData[group].ocrHotStart),
					ocrModelWriteToMemory:
						typeof newSettings?.ocrModelWriteToMemory === "boolean"
							? newSettings.ocrModelWriteToMemory
							: (prevSettings?.ocrModelWriteToMemory ??
								defaultAppSettingsData[group].ocrModelWriteToMemory),
					ocrDetectAngle:
						typeof newSettings?.ocrDetectAngle === "boolean"
							? newSettings.ocrDetectAngle
							: (prevSettings?.ocrDetectAngle ??
								defaultAppSettingsData[group].ocrDetectAngle),
					recordCaptureHistory:
						typeof newSettings?.recordCaptureHistory === "boolean"
							? newSettings.recordCaptureHistory
							: (prevSettings?.recordCaptureHistory ??
								defaultAppSettingsData[group].recordCaptureHistory),
					historyValidDuration:
						typeof newSettings?.historyValidDuration === "number"
							? (newSettings.historyValidDuration as HistoryValidDuration)
							: (prevSettings?.historyValidDuration ??
								defaultAppSettingsData[group].historyValidDuration),
					historySaveEditResult:
						typeof newSettings?.historySaveEditResult === "boolean"
							? newSettings.historySaveEditResult
							: (prevSettings?.historySaveEditResult ??
								defaultAppSettingsData[group].historySaveEditResult),
					correctColorFilter:
						typeof newSettings?.correctColorFilter === "boolean"
							? newSettings.correctColorFilter
							: (prevSettings?.correctColorFilter ??
								defaultAppSettingsData[group].correctColorFilter),
					tryWriteBitmapImageToClipboard:
						typeof newSettings?.tryWriteBitmapImageToClipboard === "boolean"
							? newSettings.tryWriteBitmapImageToClipboard
							: (prevSettings?.tryWriteBitmapImageToClipboard ??
								defaultAppSettingsData[group].tryWriteBitmapImageToClipboard),
					enableMultipleMonitor:
						typeof newSettings?.enableMultipleMonitor === "boolean"
							? newSettings.enableMultipleMonitor
							: (prevSettings?.enableMultipleMonitor ??
								defaultAppSettingsData[group].enableMultipleMonitor),
					correctHdrColor:
						typeof newSettings?.correctHdrColor === "boolean"
							? newSettings.correctHdrColor
							: (prevSettings?.correctHdrColor ??
								defaultAppSettingsData[group].correctHdrColor),
					correctHdrColorAlgorithm:
						typeof newSettings?.correctHdrColorAlgorithm === "string"
							? (newSettings.correctHdrColorAlgorithm as HdrColorAlgorithm)
							: (prevSettings?.correctHdrColorAlgorithm ??
								defaultAppSettingsData[group].correctHdrColorAlgorithm),
				};
			} else if (group === AppSettingsGroup.SystemCore) {
				newSettings = newSettings as AppSettingsData[typeof group];
				const prevSettings = appSettingsRef.current[group] as
					| AppSettingsData[typeof group]
					| undefined;

				settings = {
					hotLoadPageCount:
						typeof newSettings?.hotLoadPageCount === "number"
							? Math.max(Math.min(3, newSettings.hotLoadPageCount), 0)
							: (prevSettings?.hotLoadPageCount ??
								defaultAppSettingsData[group].hotLoadPageCount),
				};
			} else if (group === AppSettingsGroup.FunctionGlobalShortcut) {
				newSettings = newSettings as AppSettingsData[typeof group];
				const prevSettings = appSettingsRef.current[group] as
					| AppSettingsData[typeof group]
					| undefined;

				settings = {
					disableOnFocusedFullScreenWindow:
						typeof newSettings?.disableOnFocusedFullScreenWindow === "boolean"
							? newSettings.disableOnFocusedFullScreenWindow
							: (prevSettings?.disableOnFocusedFullScreenWindow ??
								defaultAppSettingsData[group].disableOnFocusedFullScreenWindow),
				};
			} else {
				return defaultAppSettingsData[group];
			}

			setAppSettings(
				{
					...appSettingsRef.current,
					[group]: settings,
				},
				ignoreState,
				ignorePublisher,
			);

			if (saveToFile) {
				if (debounce) {
					writeAppSettingsDebounce(group, settings, syncAllWindow);
				} else {
					writeAppSettings(group, settings, syncAllWindow);
				}
			}

			return settings;
		},
		[setAppSettings, isReady, writeAppSettingsDebounce, writeAppSettings],
	);

	const reloadAppSettings = useCallback(async () => {
		setAppSettingsLoadingPublisher(true);

		const groups = Object.keys(defaultAppSettingsData).filter(
			(group) => group in defaultAppSettingsData,
		);

		const settings: AppSettingsData = {} as AppSettingsData;

		// 启动时验证下目录是否存在
		try {
			await createDir(await getConfigDirPath());
		} catch (error) {
			appError(
				`[reloadAppSettings] create dir ${await getConfigDirPath()} failed`,
				error,
			);
			return;
		}

		await Promise.all(
			(groups as AppSettingsGroup[]).map(async (group) => {
				let fileContent = "";
				try {
					// 创建文件夹成功的话，文件不存在，则不读取
					fileContent = await textFileRead(await getFilePath(group));
				} catch (error) {
					appWarn(
						`[reloadAppSettings] read file ${await getFilePath(group)} failed: ${JSON.stringify(error)}`,
					);
				}

				const saveToFile = appWindowRef.current?.label === "main";

				if (!fileContent) {
					settings[group] = updateAppSettings(
						group,
						defaultAppSettingsData[group],
						false,
						saveToFile,
						false,
						true,
						true,
						// biome-ignore lint/suspicious/noExplicitAny: any is used to avoid type errors
					) as any;
					return Promise.resolve();
				}

				settings[group] = updateAppSettings(
					group,
					fileContent,
					false,
					saveToFile,
					false,
					true,
					true,
					// biome-ignore lint/suspicious/noExplicitAny: any is used to avoid type errors
				) as any;
			}),
		);

		if (isEqual(appSettingsRef.current, settings)) {
			setAppSettings(settings);
		}

		setAppSettingsLoadingPublisher(false);
	}, [setAppSettingsLoadingPublisher, updateAppSettings, setAppSettings]);

	const initedAppSettings = useRef(false);
	useEffect(() => {
		if (initedAppSettings.current) {
			return;
		}
		initedAppSettings.current = true;

		reloadAppSettings().then(() => {
			if (appWindowRef.current?.label === "main") {
				releaseDrawPage(true).then(() => {
					createDrawWindow();
				});
			}
		});
	}, [reloadAppSettings]);

	const [, antdLocale] = useMemo(() => {
		const language = appSettings[AppSettingsGroup.Common].language;
		switch (language) {
			case AppSettingsLanguage.ZHHans:
				return ["zh-CN", zhCN];
			case AppSettingsLanguage.ZHHant:
				return ["zh-TW", zhTW];
			default:
				return ["en-US", enUS];
		}
	}, [appSettings]);

	const appSettingsContextValue = useMemo(() => {
		return {
			...appSettings,
			updateAppSettings,
			reloadAppSettings,
		};
	}, [appSettings, updateAppSettings, reloadAppSettings]);

	const appSettingsTheme = useMemo(() => {
		return appSettings[AppSettingsGroup.Common].theme;
	}, [appSettings]);
	const appContextValue = useMemo(() => {
		return {
			appWindowRef,
			currentTheme:
				appSettingsTheme === AppSettingsTheme.System
					? currentSystemTheme
					: appSettingsTheme,
		};
	}, [appSettingsTheme, currentSystemTheme]);

	useEffect(() => {
		const handleUnhandledRejection = (event: PromiseRejectionEvent) => {
			try {
				const reason = event.reason;
				const { message: errorMessage, details: errorDetails } =
					formatErrorDetails(reason);

				// 添加基础的上下文信息
				const fullDetails: Record<string, unknown> = {
					reason: reason,
					timestamp: new Date().toISOString(),
					userAgent: navigator.userAgent,
					url: location.href,
					...errorDetails, // 合并错误对象的详细信息
				};

				appError(`Unhandled Promise Rejection: ${errorMessage}`, fullDetails);
			} catch {
				appError(`Failed to handle unhandled promise rejection`);
			}
		};

		const handleGlobalError = (event: ErrorEvent) => {
			try {
				const error = event.error;
				const { message: errorMessage, details: errorDetails } =
					formatErrorDetails(error);

				// 合并 ErrorEvent 的信息和错误对象的详细信息
				const fullDetails: Record<string, unknown> = {
					message: event.message,
					filename: event.filename,
					lineno: event.lineno,
					colno: event.colno,
					timestamp: new Date().toISOString(),
					userAgent: navigator.userAgent,
					url: location.href,
					...errorDetails, // 合并错误对象的详细信息
				};

				appError(`Global Error: ${errorMessage}`, fullDetails);
			} catch {
				appError(`Failed to handle global error`);
			}
		};

		window.addEventListener("unhandledrejection", handleUnhandledRejection);
		window.addEventListener("error", handleGlobalError);

		return () => {
			window.removeEventListener(
				"unhandledrejection",
				handleUnhandledRejection,
			);
			window.removeEventListener("error", handleGlobalError);
		};
	}, []);

	const antdTheme = useMemo((): ThemeConfig => {
		const algorithms = [
			appContextValue.currentTheme === AppSettingsTheme.Dark
				? theme.darkAlgorithm
				: theme.defaultAlgorithm,
		];
		if (appSettings[AppSettingsGroup.Common].enableCompactLayout) {
			algorithms.push(theme.compactAlgorithm);
		}

		return {
			algorithm: algorithms,
			token: {
				colorPrimary: appSettings[AppSettingsGroup.Common].mainColor,
				borderRadius: appSettings[AppSettingsGroup.Common].borderRadius,
			},
		};
	}, [
		appContextValue.currentTheme,
		appSettings[AppSettingsGroup.Common].enableCompactLayout,
		appSettings[AppSettingsGroup.Common].borderRadius,
		appSettings[AppSettingsGroup.Common].mainColor,
	]);

	useEffect(() => {
		document.body.className =
			appContextValue.currentTheme === AppSettingsTheme.Dark
				? "app-dark"
				: "app-light";
	}, [appContextValue.currentTheme]);

	return (
		<AppSettingsActionContext.Provider value={appSettingsContextValue}>
			<ConfigProvider theme={antdTheme} locale={antdLocale}>
				<IntlProvider
					locale={appSettings[AppSettingsGroup.Common].language}
					messages={messages[appSettings[AppSettingsGroup.Common].language]}
					defaultLocale={AppSettingsLanguage.ZHHans}
				>
					<AppContext.Provider value={appContextValue}>
						{children}
					</AppContext.Provider>
				</IntlProvider>
			</ConfigProvider>
		</AppSettingsActionContext.Provider>
	);
};

export const AppSettingsContextProvider = React.memo(
	withStatePublisher(
		withStatePublisher(AppSettingsContextProviderCore, AppSettingsPublisher),
		AppSettingsLoadingPublisher,
	),
);
