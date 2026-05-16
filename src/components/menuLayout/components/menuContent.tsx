import { CloseOutlined, MinusOutlined } from "@ant-design/icons";
import {
	type Window as AppWindow,
	getCurrentWindow,
} from "@tauri-apps/api/window";
import * as tauriOs from "@tauri-apps/plugin-os";
import { Button, Layout, Space, theme } from "antd";
import { Header } from "antd/es/layout/layout";
import React, { useEffect, useMemo, useRef, useState } from "react";
import RSC from "react-scrollbars-custom";
import { PageNav } from "@/components/pageNav";
import type { RouteMapItem } from "@/types/components/menuLayout";

const { Content } = Layout;

const MenuContentCore: React.FC<{
	pathname: string;
	routeTabsMap: Record<string, RouteMapItem>;
	children: React.ReactNode;
}> = ({ pathname, routeTabsMap, children }) => {
	const { token } = theme.useToken();
	const appWindowRef = useRef<AppWindow | undefined>(undefined);
	useEffect(() => {
		appWindowRef.current = getCurrentWindow();
	}, []);

	const tabItems = useMemo(() => {
		return routeTabsMap[pathname] ?? routeTabsMap["/"] ?? [];
	}, [pathname, routeTabsMap]);

	const contentRef = useRef<HTMLDivElement>(null);

	const [currentPlatform, setCurrentPlatform] = useState<
		tauriOs.Platform | undefined
	>(undefined);
	useEffect(() => {
		setCurrentPlatform(tauriOs.platform());
	}, []);

	return (
		<Layout>
			<Header data-tauri-drag-region className="app-tauri-drag-region">
				{currentPlatform !== "macos" && (
					<Space>
						<Button
							type="text"
							size="small"
							icon={<MinusOutlined />}
							onClick={() => {
								appWindowRef.current?.minimize();
							}}
						/>
						<Button
							type="text"
							size="small"
							icon={<CloseOutlined />}
							onClick={() => {
								appWindowRef.current?.hide();
								appWindowRef.current?.emit("on-hide-main-window");
							}}
						/>
					</Space>
				)}

				{currentPlatform === "macos" && (
					<div data-tauri-drag-region className="logo-text">
						<div data-tauri-drag-region className="logo-text-highlight">
							Snow
						</div>
						<div data-tauri-drag-region>Shot</div>
					</div>
				)}
			</Header>
			<Content>
				<div className="content-wrap">
					<div data-tauri-drag-region className="app-tauri-drag-region"></div>
					<div data-tauri-drag-region className="app-tauri-drag-region"></div>
					<div data-tauri-drag-region className="app-tauri-drag-region"></div>
					<div data-tauri-drag-region className="app-tauri-drag-region"></div>
					<div className="center">
						<PageNav tabItems={tabItems} />
						<RSC>
							<div ref={contentRef} className="content-container">
								{children}
							</div>
						</RSC>
					</div>
					<div data-tauri-drag-region className="app-tauri-drag-region"></div>
					<div data-tauri-drag-region className="app-tauri-drag-region"></div>
					<div data-tauri-drag-region className="app-tauri-drag-region"></div>
					<div data-tauri-drag-region className="app-tauri-drag-region"></div>
				</div>
			</Content>

			<style jsx>{`
                .content-wrap {
                    display: grid;
                    grid-template-columns: ${token.padding}px auto ${token.padding}px;
                    grid-template-rows: ${token.padding}px auto ${token.padding}px;
                    height: 100%;
                }

                .content-wrap .center {
                    grid-column: 2;
                    grid-row: 2;
                    overflow-y: hidden;
                    overflow-x: hidden;
                    border-radius: ${token.borderRadiusLG}px;
                    background: ${token.colorBgContainer};
                    padding: ${token.padding}px ${token.borderRadiusLG}px;
                    display: flex;
                    flex-direction: column;
                    transform: translateY(0px);
                }

                .content-wrap .center::-webkit-scrollbar {
                    display: none;
                }

                .content-container {
                    padding: 0 ${token.padding}px;
                    width: 100%;
                    height: 100%;
                    overflow-x: hidden;
                }

                .logo-text {
                    position: absolute;
                    line-height: initial;
                    display: flex;
                    height: 32px;
                    align-items: center;
                    justify-content: center;
                    color: var(--snow-shot-text-color);
                    font-style: italic;
                    font-weight: 600;
                    user-select: none;
                    /* 对齐系统里的 title 位置 */
                    position: absolute;
                    left: 0;
                    right: 0;
                }

                .logo-text-highlight {
                    color: var(--snow-shot-purple-color);
                }
            `}</style>
		</Layout>
	);
};

export const MenuContent = React.memo(MenuContentCore);
