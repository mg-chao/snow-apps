import {
	AppstoreOutlined,
	InfoCircleOutlined,
	SettingOutlined,
	ToolOutlined,
} from "@ant-design/icons";
import { useLocation, useRouter } from "@tanstack/react-router";
import { Layout, theme } from "antd";
import type { ItemType, MenuItemType } from "antd/es/menu/interface";
import React, { useCallback, useContext, useEffect, useMemo } from "react";
import { useIntl } from "react-intl";
import { CheckEnvironment } from "@/components/checkEnvironment";
import { CheckVersion } from "@/components/checkVersion";
import { GlobalEventHandler } from "@/components/globalEventHandler";
import { GlobalShortcut } from "@/components/globalShortcut";
import { PersonalizationIcon } from "@/components/icons";
import { InitService } from "@/components/initService";
import {
	TrayIconLoader,
	TrayIconStatePublisher,
} from "@/components/trayIconLoader";
import {
	PLUGIN_ID_AI_CHAT,
	PLUGIN_ID_FFMPEG,
	PLUGIN_ID_RAPID_OCR,
	PLUGIN_ID_TRANSLATE,
} from "@/constants/pluginService";
import { AppContext } from "@/contexts/appContext";
import { AppSettingsActionContext } from "@/contexts/appSettingsActionContext";
import { usePluginServiceContext } from "@/contexts/pluginServiceContext";
import { useAppSettingsLoad } from "@/hooks/useAppSettingsLoad";
import { withStatePublisher } from "@/hooks/useStatePublisher";
import { en } from "@/messages/en";
import { zhHans } from "@/messages/zhHans";
import { zhHant } from "@/messages/zhHant";
import {
	AppSettingsGroup,
	AppSettingsLanguage,
	AppSettingsTheme,
} from "@/types/appSettings";
import type { RouteItem, RouteMapItem } from "@/types/components/menuLayout";
import { getPlatformValue } from "@/utils/platform";
import { MenuContent } from "./components/menuContent";
import { MenuSider } from "./components/menuSider";

type MenuItem = ItemType<MenuItemType>;

const MenuLayoutCore: React.FC<{ children: React.ReactNode }> = ({
	children,
}) => {
	useEffect(() => {
		if (process.env.NODE_ENV !== "development") {
			return;
		}

		const zhHansKeys = Object.keys(zhHans);
		const zhHantKeys = new Set(Object.keys(zhHant));
		const enKeys = new Set(Object.keys(en));

		const zhHantMissingKeys: Record<string, string> = {};
		zhHansKeys
			.filter((key) => !zhHantKeys.has(key))
			.forEach((key) => {
				zhHantMissingKeys[key] = zhHans[key as keyof typeof zhHans];
			});

		const enMissingKeys: Record<string, string> = {};
		zhHansKeys
			.filter((key) => !enKeys.has(key))
			.forEach((key) => {
				enMissingKeys[key] = zhHans[key as keyof typeof zhHans];
			});

		console.log("App zh-Hant missing messages: ", zhHantMissingKeys);
		console.log("App en missing messages: ", enMissingKeys);
	}, []);

	const intl = useIntl();
	const appSettings = useContext(AppSettingsActionContext);
	const { currentTheme } = useContext(AppContext);
	const { updateAppSettings } = appSettings;

	const routerLocation = useLocation();
	const pathname = routerLocation.pathname || "/";
	useAppSettingsLoad(
		useCallback(
			(settings) => {
				// 获取浏览器语言，判断是否需要切换语言
				const settingBrowserLanguage =
					settings[AppSettingsGroup.Common].browserLanguage;
				const browserLanguage = navigator.language;
				if (settingBrowserLanguage !== browserLanguage) {
					// 切换语言
					let language = AppSettingsLanguage.EN;
					if (browserLanguage.startsWith("zh")) {
						if (browserLanguage.startsWith("zh-TW")) {
							language = AppSettingsLanguage.ZHHant;
						} else {
							language = AppSettingsLanguage.ZHHans;
						}
					}

					updateAppSettings(
						AppSettingsGroup.Common,
						{
							browserLanguage: browserLanguage,
							language,
						},
						false,
						true,
						true,
					);
				}
			},
			[updateAppSettings],
		),
	);

	const { token } = theme.useToken();
	const { isReadyStatus } = usePluginServiceContext();
	const router = useRouter();
	const routes = useMemo(() => {
		const routes: RouteItem[] = [
			{
				key: "/",
				path: "/",
				label: intl.formatMessage({ id: "menu.functions" }),
				icon: <AppstoreOutlined />,
				tabs: [
					{
						key: "screenshotFunction",
						label: intl.formatMessage({ id: "home.screenshotFunction" }),
					},
					{
						key: "chatFunction",
						label: intl.formatMessage({ id: "home.chatFunction" }),
					},
					{
						key: "translationFunction",
						label: intl.formatMessage({ id: "home.translationFunction" }),
					},
					{
						key: "videoRecordFunction",
						label: intl.formatMessage({ id: "home.videoRecordFunction" }),
					},
					{
						key: "otherFunction",
						label: intl.formatMessage({ id: "home.otherFunction" }),
					},
				].filter((item) => {
					if (item.key === "videoRecordFunction") {
						return isReadyStatus?.(PLUGIN_ID_FFMPEG);
					}

					if (item.key === "chatFunction") {
						return isReadyStatus?.(PLUGIN_ID_AI_CHAT);
					}

					if (item.key === "translationFunction") {
						return isReadyStatus?.(PLUGIN_ID_TRANSLATE);
					}

					return true;
				}),
			},
			{
				key: "/tools",
				path: undefined,
				label: intl.formatMessage({ id: "menu.tools" }),
				icon: <ToolOutlined />,
				tabs: [],
				children: [
					{
						key: "/tools/translation",
						path: "/tools/translation",
						label: intl.formatMessage({ id: "menu.tools.translation" }),
						hideTabs: true,
						tabs: [
							{
								key: "translation",
								label: intl.formatMessage({ id: "menu.tools.translation" }),
							},
						],
					},
					{
						key: "/tools/chat",
						path: "/tools/chat",
						label: intl.formatMessage({ id: "menu.tools.chat" }),
						hideTabs: true,
						tabs: [
							{
								key: "chat",
								label: intl.formatMessage({ id: "menu.tools.chat" }),
							},
						],
					},
					{
						key: "/tools/captureHistory",
						path: "/tools/captureHistory",
						label: intl.formatMessage({ id: "menu.tools.captureHistory" }),
						hideTabs: true,
						tabs: [
							{
								key: "captureHistory",
								label: intl.formatMessage({ id: "menu.tools.captureHistory" }),
							},
						],
					},
				].filter((item) => {
					if (item.key === "/tools/chat") {
						return isReadyStatus?.(PLUGIN_ID_AI_CHAT);
					}

					if (item.key === "/tools/translation") {
						return isReadyStatus?.(PLUGIN_ID_TRANSLATE);
					}

					return true;
				}),
			},
			{
				key: "/personalization",
				path: undefined,
				label: intl.formatMessage({ id: "menu.personalization" }),
				icon: <PersonalizationIcon />,
				tabs: [],
				children: [
					{
						key: "/personalization/appearance",
						path: "/personalization/appearance",
						label: intl.formatMessage({
							id: "menu.personalization.appearance",
						}),
						hideTabs: true,
						tabs: [
							{
								key: "appearance",
								label: intl.formatMessage({
									id: "menu.personalization.appearance",
								}),
							},
						],
					},
					{
						key: "/personalization/plugins",
						path: "/personalization/plugins",
						label: intl.formatMessage({ id: "menu.personalization.plugins" }),
						hideTabs: true,
						tabs: [
							{
								key: "plugins",
								label: intl.formatMessage({
									id: "menu.personalization.plugins",
								}),
							},
						],
					},
				],
			},
			{
				key: "/settings",
				path: undefined,
				label: intl.formatMessage({ id: "menu.settings" }),
				icon: <SettingOutlined />,
				tabs: [],
				children: [
					{
						key: "/settings/generalSettings",
						path: "/settings/generalSettings",
						label: intl.formatMessage({ id: "menu.settings.generalSettings" }),
						tabs: [
							{
								key: "commonSettings",
								label: intl.formatMessage({ id: "settings.commonSettings" }),
							},
							{
								key: "screenshotSettings",
								label: intl.formatMessage({
									id: "settings.screenshotSettings",
								}),
							},
							{
								key: "fixedContentSettings",
								label: intl.formatMessage({
									id: "settings.fixedContentSettings",
								}),
							},
							{
								key: "trayIconSettings",
								label: intl.formatMessage({
									id: "settings.commonSettings.trayIconSettings",
								}),
							},
						],
					},
					{
						key: "/settings/functionSettings",
						path: "/settings/functionSettings",
						label: intl.formatMessage({ id: "menu.settings.functionSettings" }),
						tabs: [
							{
								key: "screenshotSettings",
								label: intl.formatMessage({
									id: "settings.functionSettings.screenshotSettings",
								}),
							},
							{
								key: "functionDrawSettings",
								label: intl.formatMessage({
									id: "settings.commonSettings.draw",
								}),
							},
							{
								key: "fixedContentSettings",
								label: intl.formatMessage({
									id: "settings.functionSettings.fixedContentSettings",
								}),
							},
							{
								key: "ocrSettings",
								label: intl.formatMessage({
									id: "settings.functionSettings.ocrSettings",
								}),
							},
							{
								key: "translationSettings",
								label: intl.formatMessage({
									id: "settings.functionSettings.translationSettings",
								}),
							},
							{
								key: "chatSettings",
								label: intl.formatMessage({
									id: "settings.functionSettings.chatSettings",
								}),
							},
							{
								key: "fullScreenDrawSettings",
								label: intl.formatMessage({
									id: "settings.functionSettings.fullScreenDrawSettings",
								}),
							},
							{
								key: "videoRecordSettings",
								label: intl.formatMessage({
									id: "settings.functionSettings.videoRecordSettings",
								}),
							},
							{
								key: "trayIconSettings",
								label: intl.formatMessage({
									id: "settings.functionSettings.trayIconSettings",
								}),
							},
							{
								key: "globalShortcutSettings",
								label: intl.formatMessage({
									id: "settings.functionSettings.globalShortcutSettings",
								}),
							},
							{
								key: "outputSettings",
								label: intl.formatMessage({
									id: "settings.functionSettings.outputSettings",
								}),
							},
						].filter((item) => {
							if (item.key === "videoRecordSettings") {
								return isReadyStatus?.(PLUGIN_ID_FFMPEG);
							}

							if (item.key === "translationSettings") {
								return isReadyStatus?.(PLUGIN_ID_TRANSLATE);
							}

							if (item.key === "chatSettings") {
								return isReadyStatus?.(PLUGIN_ID_AI_CHAT);
							}

							if (item.key === "ocrSettings") {
								return isReadyStatus?.(PLUGIN_ID_RAPID_OCR);
							}

							return true;
						}),
					},
					{
						key: "/settings/hotKeySettings",
						path: "/settings/hotKeySettings",
						label: intl.formatMessage({ id: "menu.settings.hotKeySettings" }),
						tabs: [
							{
								key: "translation",
								label: intl.formatMessage({
									id: "settings.hotKeySettings.translation",
								}),
							},
							{
								key: "chat",
								label: intl.formatMessage({
									id: "settings.hotKeySettings.chat",
								}),
							},
							{
								key: "fixedContent",
								label: intl.formatMessage({
									id: "settings.hotKeySettings.fixedContent",
								}),
							},
							{
								key: "drawingHotKey",
								label: intl.formatMessage({ id: "settings.drawingHotKey" }),
							},
						].filter((item) => {
							if (item.key === "chat") {
								return isReadyStatus?.(PLUGIN_ID_AI_CHAT);
							}

							if (item.key === "translation") {
								return isReadyStatus?.(PLUGIN_ID_TRANSLATE);
							}

							return true;
						}),
					},
					{
						key: "/settings/systemSettings",
						path: "/settings/systemSettings",
						label: intl.formatMessage({ id: "menu.settings.systemSettings" }),
						tabs: [
							{
								key: "commonSettings",
								label: intl.formatMessage({ id: "settings.commonSettings" }),
							},
							...getPlatformValue(
								[],
								[
									{
										key: "macosPermissionsSettings",
										label: intl.formatMessage({
											id: "settings.systemSettings.macosPermissionsSettings",
										}),
									},
								],
							),
							{
								key: "screenshotSettings",
								label: intl.formatMessage({
									id: "settings.systemSettings.screenshotSettings",
								}),
							},
							{
								key: "networkSettings",
								label: intl.formatMessage({
									id: "settings.systemSettings.networkSettings",
								}),
							},
							{
								key: "scrollScreenshotSettings",
								label: intl.formatMessage({
									id: "settings.systemSettings.scrollScreenshotSettings",
								}),
							},
							{
								key: "chatSettings",
								label: intl.formatMessage({ id: "settings.chatSettings" }),
							},
							{
								key: "coreSettings",
								label: intl.formatMessage({
									id: "settings.systemSettings.coreSettings",
								}),
							},
							{
								key: "dataFile",
								label: intl.formatMessage({
									id: "settings.systemSettings.dataFile",
								}),
							},
						].filter(() => {
							// if (item.key === 'chatSettings') {
							//     return isReadyStatus?.(PLUGIN_ID_AI_CHAT);
							// }

							return true;
						}),
					},
				],
			},
			{
				key: "/about",
				path: "/about",
				tabs: [
					{
						key: "snowShot",
						label: intl.formatMessage({ id: "home.snowShot" }),
					},
				],
				label: intl.formatMessage({ id: "menu.about" }),
				icon: <InfoCircleOutlined />,
			},
		];

		return routes;
	}, [intl, isReadyStatus]);
	const { menuItems, routeTabsMap } = useMemo(() => {
		const routeTabsMap: Record<string, RouteMapItem> = {};

		const convertToMenuItem = (route: RouteItem): MenuItem => {
			const menuItem: MenuItem = {
				key: route.key,
				label: route.label,
				icon: route.icon,
				onClick: () => {
					if (!route.path) {
						return;
					}

					router.navigate({ to: route.path });
				},
				children: undefined as unknown as MenuItem[],
			};

			if (route.children?.length) {
				menuItem.children = route.children.map((child) =>
					convertToMenuItem(child),
				);
			}

			if (route.path && route.tabs?.length !== undefined) {
				routeTabsMap[route.path] = {
					items: route.tabs,
					hideTabs: route.hideTabs,
				};
			}

			return menuItem;
		};

		const menuItems = Object.values(routes).map(convertToMenuItem);

		return { menuItems, routeTabsMap };
	}, [router, routes]);

	return (
		<>
			<TrayIconLoader />
			<GlobalEventHandler />
			<CheckEnvironment />
			<CheckVersion />
			<div className="menu-layout-wrap">
				<Layout>
					<MenuSider
						menuItems={menuItems}
						darkMode={currentTheme === AppSettingsTheme.Dark}
						pathname={pathname}
					/>
					<MenuContent pathname={pathname} routeTabsMap={routeTabsMap}>
						<GlobalShortcut>{children}</GlobalShortcut>
					</MenuContent>
				</Layout>
				<style jsx>{`
                    .menu-layout-wrap {
                        box-shadow: 0 0 12px 0 rgba(0, 0, 0, 0.21);
                        overflow: hidden;
                        height: 100%;
                        position: fixed;
                        top: 0;
                        left: 0;
                        right: 0;
                        bottom: 0;
                        z-index: 1;
                    }

                    .menu-layout-wrap :global(.ant-layout) {
                        height: 100% !important;
                    }

                    .menu-layout-wrap > :global(.ant-layout) {
                        flex-direction: row !important;
                    }

                    .menu-layout-wrap :global(.ant-layout-sider-trigger) {
                        position: absolute !important;
                    }

                    .menu-layout-wrap :global(.ant-layout-sider) {
                        box-shadow: ${token.boxShadowSecondary};
                    }

                    .menu-layout-wrap :global(.ant-layout-header) {
                        height: 32px !important;
                        background: ${token.colorBgContainer};
                        display: flex;
                        align-items: center;
                        justify-content: flex-end;
                        padding: 0 ${token.padding}px;
                    }
                `}</style>
			</div>
		</>
	);
};

const ML = React.memo(
	withStatePublisher(MenuLayoutCore, TrayIconStatePublisher),
);

export const MenuLayout = ({ children }: { children: React.ReactNode }) => {
	return (
		<>
			<InitService />
			<ML>{children}</ML>
		</>
	);
};
