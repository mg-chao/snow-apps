import { Tabs, theme } from "antd";
import { useEffect, useRef, useState } from "react";
import type { RouteMapItem } from "@/types/components/menuLayout";

export const PageNav: React.FC<{
	tabItems: RouteMapItem;
}> = ({ tabItems }) => {
	const { token } = theme.useToken();

	const [activeKey, setActiveKey] = useState<string | undefined>(
		tabItems.items?.[0]?.key,
	);

	// 用 IntersectionObserver 跟踪每个 section 的可见性。
	// 选当前在视口顶部 active band 内最靠上的 section 作为 active；
	// 若 active band 内没有 section，则 fallback 到"已经滑过去且最近顶部"的那个，
	// 防止滚动卡在两个 section 之间时 active 状态空白。
	const visibilityMapRef = useRef<Map<string, IntersectionObserverEntry>>(
		new Map(),
	);

	useEffect(() => {
		if (typeof IntersectionObserver === "undefined") {
			return;
		}
		const tabs = tabItems.items;
		if (!tabs || tabs.length === 0) {
			return;
		}
		setActiveKey(tabs[0].key as string);
		visibilityMapRef.current.clear();

		const computeActive = () => {
			let bestIntersecting: { key: string; top: number } | undefined;
			let lastScrolledPast: { key: string; top: number } | undefined;
			for (const [key, entry] of visibilityMapRef.current) {
				const top = entry.boundingClientRect.top;
				if (entry.isIntersecting) {
					if (!bestIntersecting || top < bestIntersecting.top) {
						bestIntersecting = { key, top };
					}
				} else if (top < 0) {
					if (!lastScrolledPast || top > lastScrolledPast.top) {
						lastScrolledPast = { key, top };
					}
				}
			}
			const next = bestIntersecting ?? lastScrolledPast;
			if (next) {
				setActiveKey(next.key);
			}
		};

		const observer = new IntersectionObserver(
			(entries) => {
				for (const entry of entries) {
					const id = (entry.target as HTMLElement).id;
					if (id) {
						visibilityMapRef.current.set(id, entry);
					}
				}
				computeActive();
			},
			{
				// 视口顶部 30% 作为 "active band"：section 顶部进入这一带才算 active
				rootMargin: "0px 0px -70% 0px",
				threshold: 0,
			},
		);

		// 元素在 PageNav 挂载时可能还没渲染上，rAF 重试至全部就位
		let raf: number | null = null;
		const tryObserve = () => {
			let allFound = true;
			for (const item of tabs) {
				const el = document.getElementById(item.key as string);
				if (el) {
					observer.observe(el);
				} else {
					allFound = false;
				}
			}
			if (!allFound) {
				raf = requestAnimationFrame(tryObserve);
			}
		};
		tryObserve();

		return () => {
			if (raf != null) {
				cancelAnimationFrame(raf);
			}
			observer.disconnect();
			visibilityMapRef.current.clear();
		};
	}, [tabItems]);

	return (
		<div
			className="page-nav"
			style={{ display: tabItems.hideTabs ? "none" : undefined }}
		>
			<Tabs
				activeKey={activeKey}
				items={tabItems.items}
				size="small"
				onChange={(key) => {
					const target = document.getElementById(key);
					if (!target) {
						return;
					}
					target.scrollIntoView({ behavior: "smooth" });
					setActiveKey(key);
				}}
			/>

			<style jsx>{`
                .page-nav :global(.ant-tabs) {
                    margin-top: -12px !important;
                    padding: 0 ${token.padding}px !important;
                }

                .page-nav :global(.ant-tabs-nav-wrap) {
                    height: 32px !important;
                }
            `}</style>
		</div>
	);
};
