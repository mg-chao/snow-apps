import Color from "color";
import type { ElementRect } from "@/types/commands/screenshot";
import { DrawState } from "@/types/draw";
import { MousePosition } from "@/utils/mousePosition";

export enum SelectState {
	/** 自动选择 */
	Auto = 0,
	/** 手动选择 */
	Manual = 1,
	/** 拖动 */
	Drag = 2,
	/** Scroll Resize */
	ScrollResize = 3,
	/** 已选择 */
	Selected = 10,
}

export enum DragMode {
	/** 拖动整体 */
	All = 0,
	/** 拖动左上角 */
	TopLeft = 1,
	/** 拖动上边 */
	Top = 2,
	/** 拖动右上角 */
	TopRight = 3,
	/** 拖动右边 */
	Right = 4,
	/** 拖动右下角 */
	BottomRight = 5,
	/** 拖动下边 */
	Bottom = 6,
	/** 拖动左下角 */
	BottomLeft = 7,
	/** 拖动左边 */
	Left = 8,
}

// 全屏遮罩的透明度
const MASK_OPACITY = 0.5;
// 全屏遮罩圆形 controls 点的宽度
export const MASK_CIRCLE_CONTROL_WIDTH = 4;
// 全屏遮罩圆形 controls 点的描边宽度
export const MASK_CIRCLE_CONTROL_STROKE_WIDTH = 1.5;
// 全屏遮罩圆形 controls 点显示控制点的宽度
export const MASK_CIRCLE_CONTROL_SHOW_END_CONTROL_WIDTH = 32;
// 全屏遮罩圆形 controls 点显示中点控制点的宽度
export const MASK_CIRCLE_CONTROL_SHOW_MID_CONTROL_WIDTH = 64;
// 全屏遮罩的 mask 的描边宽度
export const MASK_CONTROL_BORDER_STROKE_WIDTH = 2;

// 全屏辅助线相关常量
export const AUXILIARY_LINE_WIDTH = 1;
export const AUXILIARY_LINE_DASH = [10, 3];

const LIGHT_MASK_BACKGROUND_COLOR = "#00000080";
const DARK_MASK_BACKGROUND_COLOR = Color("#434343")
	.alpha(MASK_OPACITY)
	.toString();

export const getMaskBackgroundColor = (darkMode: boolean) => {
	return darkMode ? DARK_MASK_BACKGROUND_COLOR : LIGHT_MASK_BACKGROUND_COLOR;
};

const drawCircleControl = (
	canvasContext: CanvasRenderingContext2D,
	x: number,
	y: number,
	maskCircleControlWidth: number,
) => {
	canvasContext.moveTo(x + maskCircleControlWidth, y);
	canvasContext.arc(x, y, maskCircleControlWidth, 0, Math.PI * 2);
};

export const drawSelectRect = (
	themeConfig: {
		strokeColor: string;
		controlStrokeColor: string;
		controlFillColor: string;
	},
	monitorWidth: number,
	monitorHeight: number,
	selectRect: ElementRect,
	activeMonitorRect: ElementRect,
	radius: number,
	canvasContext: CanvasRenderingContext2D,
	darkMode: boolean,
	scaleFactor: number,
	hideControls: boolean,
	drawState?: DrawState,
	drawElementMask?: {
		imageData: ImageData;
	},
	fullScreenAuxiliaryLine?: {
		mousePosition: MousePosition;
		color: string;
	},
	monitorCenterAuxiliaryLine?: {
		color: string;
	},
	selectRectMaskColor?: string,
) => {
	const {
		min_x: rectMinX,
		min_y: rectMinY,
		max_x: rectMaxX,
		max_y: rectMaxY,
	} = selectRect;
	const rectWidth = rectMaxX - rectMinX;
	const rectHeight = rectMaxY - rectMinY;
	const minWidth = Math.min(rectWidth, rectHeight);

	const maskCircleControlWidth = Math.floor(
		MASK_CIRCLE_CONTROL_WIDTH * scaleFactor,
	);
	const maskCircleControlStrokeWidth = Math.floor(
		MASK_CIRCLE_CONTROL_STROKE_WIDTH * scaleFactor,
	);
	const maskControlBorderStrokeWidth = Math.floor(
		MASK_CONTROL_BORDER_STROKE_WIDTH * scaleFactor,
	);
	const maskCircleControlShowEndWidth = Math.floor(
		MASK_CIRCLE_CONTROL_SHOW_END_CONTROL_WIDTH * scaleFactor,
	);
	const maskCircleControlShowMidWidth = Math.floor(
		MASK_CIRCLE_CONTROL_SHOW_MID_CONTROL_WIDTH * scaleFactor,
	);

	if (drawState === DrawState.ScanQrcode) {
		radius = 0;
	}

	if (drawElementMask) {
		canvasContext.putImageData(drawElementMask.imageData, 0, 0);
	}

	// 使用 'copy' 模式直接替换所有内容（包括 alpha 通道），无需初始化时 clearRect
	canvasContext.globalCompositeOperation = "copy";
	canvasContext.fillStyle =
		selectRectMaskColor ?? getMaskBackgroundColor(darkMode);
	canvasContext.fillRect(0, 0, monitorWidth, monitorHeight);
	// 恢复默认模式
	canvasContext.globalCompositeOperation = "source-over";

	// 防止滚动截图的时候截取到选区边缘
	const halfWidth = Math.floor(maskControlBorderStrokeWidth * 0.5);
	const doubleHalfWidth = halfWidth * 2;
	if (radius > 0 && drawState !== DrawState.ScrollScreenshot) {
		// 清除圆角矩形区域
		// 保存当前上下文状态
		canvasContext.save();

		// 创建圆角矩形路径
		canvasContext.beginPath();
		canvasContext.roundRect(
			rectMinX - halfWidth,
			rectMinY - halfWidth,
			rectWidth + doubleHalfWidth,
			rectHeight + doubleHalfWidth,
			radius,
		);
		canvasContext.clip();

		canvasContext.clearRect(
			rectMinX - halfWidth,
			rectMinY - halfWidth,
			rectWidth + doubleHalfWidth,
			rectHeight + doubleHalfWidth,
		);

		// 恢复上下文状态
		canvasContext.restore();
	} else {
		canvasContext.clearRect(
			rectMinX - halfWidth,
			rectMinY - halfWidth,
			rectWidth + doubleHalfWidth,
			rectHeight + doubleHalfWidth,
		);
	}

	if (drawState === DrawState.ScrollScreenshot) {
		return;
	}

	if (fullScreenAuxiliaryLine || monitorCenterAuxiliaryLine) {
		canvasContext.setLineDash(
			AUXILIARY_LINE_DASH.map((dash) => Math.floor(dash * scaleFactor)),
		);
		canvasContext.lineWidth = Math.floor(AUXILIARY_LINE_WIDTH * scaleFactor);

		if (fullScreenAuxiliaryLine) {
			const { mouseX, mouseY } = fullScreenAuxiliaryLine.mousePosition;

			canvasContext.strokeStyle = fullScreenAuxiliaryLine.color;

			canvasContext.beginPath();
			// 绘制垂直线
			canvasContext.moveTo(mouseX, activeMonitorRect.min_y);
			canvasContext.lineTo(mouseX, activeMonitorRect.max_y);
			// 绘制水平线
			canvasContext.moveTo(activeMonitorRect.min_x, mouseY);
			canvasContext.lineTo(activeMonitorRect.max_x, mouseY);
			canvasContext.stroke();
		}

		if (monitorCenterAuxiliaryLine) {
			const { color } = monitorCenterAuxiliaryLine;
			const centerX =
				activeMonitorRect.min_x +
				Math.floor((activeMonitorRect.max_x - activeMonitorRect.min_x) / 2);
			const centerY =
				activeMonitorRect.min_y +
				Math.floor((activeMonitorRect.max_y - activeMonitorRect.min_y) / 2);

			canvasContext.strokeStyle = color;

			canvasContext.beginPath();
			// 绘制垂直线
			canvasContext.moveTo(centerX, activeMonitorRect.min_y);
			canvasContext.lineTo(centerX, activeMonitorRect.max_y);
			// 绘制水平线
			canvasContext.moveTo(activeMonitorRect.min_x, centerY);
			canvasContext.lineTo(activeMonitorRect.max_x, centerY);

			canvasContext.stroke();
		}

		canvasContext.setLineDash([]);
	}

	canvasContext.strokeStyle = themeConfig.strokeColor;
	canvasContext.lineWidth = maskControlBorderStrokeWidth;

	canvasContext.beginPath();
	if (radius > 0) {
		canvasContext.roundRect(rectMinX, rectMinY, rectWidth, rectHeight, radius);
	} else {
		canvasContext.rect(rectMinX, rectMinY, rectWidth, rectHeight);
	}
	canvasContext.stroke();

	if (!hideControls) {
		const controlFillColor = themeConfig.controlFillColor;

		canvasContext.beginPath();
		if (minWidth > maskCircleControlShowEndWidth) {
			// 左上角
			drawCircleControl(
				canvasContext,
				rectMinX,
				rectMinY,
				maskCircleControlWidth,
			);
			// 右上角
			drawCircleControl(
				canvasContext,
				rectMaxX,
				rectMinY,
				maskCircleControlWidth,
			);
			// 左下角
			drawCircleControl(
				canvasContext,
				rectMinX,
				rectMaxY,
				maskCircleControlWidth,
			);
			// 右下角
			drawCircleControl(
				canvasContext,
				rectMaxX,
				rectMaxY,
				maskCircleControlWidth,
			);
		}

		if (minWidth > maskCircleControlShowMidWidth) {
			const centerX = rectMinX + Math.floor((rectMaxX - rectMinX) / 2);
			const centerY = rectMinY + Math.floor((rectMaxY - rectMinY) / 2);

			// 上边中点
			drawCircleControl(
				canvasContext,
				centerX,
				rectMinY,
				maskCircleControlWidth,
			);
			// 下边中点
			drawCircleControl(
				canvasContext,
				centerX,
				rectMaxY,
				maskCircleControlWidth,
			);
			// 左边中点
			drawCircleControl(
				canvasContext,
				rectMinX,
				centerY,
				maskCircleControlWidth,
			);
			// 右边中点
			drawCircleControl(
				canvasContext,
				rectMaxX,
				centerY,
				maskCircleControlWidth,
			);
		}

		canvasContext.fillStyle = controlFillColor;
		canvasContext.fill();
		canvasContext.strokeStyle = themeConfig.controlStrokeColor;
		canvasContext.lineWidth = maskCircleControlStrokeWidth;
		canvasContext.stroke();
	}
};

export const convertDragModeToCursor = (dragMode: DragMode) => {
	switch (dragMode) {
		case DragMode.All:
			return "move";
		case DragMode.TopLeft:
			return "nw-resize";
		case DragMode.TopRight:
			return "ne-resize";
		case DragMode.BottomLeft:
			return "sw-resize";
		case DragMode.BottomRight:
			return "se-resize";
		case DragMode.Top:
			return "n-resize";
		case DragMode.Bottom:
			return "s-resize";
		case DragMode.Left:
			return "w-resize";
		case DragMode.Right:
			return "e-resize";
		default:
			return "auto";
	}
};

// 缓存常用的边缘检测容差值
export const EDGE_DETECTION_TOLERANCE = 8;
export const getDragModeFromMousePosition = (
	selectRect: ElementRect,
	mousePosition: MousePosition,
) => {
	const tolerance = EDGE_DETECTION_TOLERANCE * window.devicePixelRatio;

	const {
		min_x: rectMinX,
		min_y: rectMinY,
		max_x: rectMaxX,
		max_y: rectMaxY,
	} = selectRect;
	const { mouseX, mouseY } = mousePosition;

	// 使用位运算进行更快的位置判断
	let position = 0;

	// 位掩码: 0b0000, 代表 [top, right, bottom, left]
	if (mouseY <= rectMinY + tolerance) position |= 0b1000;
	if (mouseX >= rectMaxX - tolerance) position |= 0b0100;
	if (mouseY >= rectMaxY - tolerance) position |= 0b0010;
	if (mouseX <= rectMinX + tolerance) position |= 0b0001;

	// 使用映射表快速确定拖动模式
	switch (position) {
		case 0b1001:
			return DragMode.TopLeft; // 左上角
		case 0b1100:
			return DragMode.TopRight; // 右上角
		case 0b0011:
			return DragMode.BottomLeft; // 左下角
		case 0b0110:
			return DragMode.BottomRight; // 右下角
		case 0b1000:
			return DragMode.Top; // 上边
		case 0b0010:
			return DragMode.Bottom; // 下边
		case 0b0001:
			return DragMode.Left; // 左边
		case 0b0100:
			return DragMode.Right; // 右边
		default:
			return DragMode.All; // 内部区域
	}
};

// 投影模式 hit-test：将整个 viewport 外部映射为 8 个 snap 区域
// 鼠标在 rect 内部仍返回 DragMode.All（保留"拖整体"语义）
export const getDragModeFromMousePositionProjection = (
	selectRect: ElementRect,
	mousePosition: MousePosition,
) => {
	const {
		min_x: rectMinX,
		min_y: rectMinY,
		max_x: rectMaxX,
		max_y: rectMaxY,
	} = selectRect;
	const { mouseX, mouseY } = mousePosition;

	let position = 0;
	// 位掩码 0b[top, right, bottom, left]
	if (mouseY < rectMinY) position |= 0b1000;
	if (mouseX > rectMaxX) position |= 0b0100;
	if (mouseY > rectMaxY) position |= 0b0010;
	if (mouseX < rectMinX) position |= 0b0001;

	switch (position) {
		case 0b1001:
			return DragMode.TopLeft;
		case 0b1100:
			return DragMode.TopRight;
		case 0b0011:
			return DragMode.BottomLeft;
		case 0b0110:
			return DragMode.BottomRight;
		case 0b1000:
			return DragMode.Top;
		case 0b0010:
			return DragMode.Bottom;
		case 0b0001:
			return DragMode.Left;
		case 0b0100:
			return DragMode.Right;
		default:
			return DragMode.All; // 鼠标在 rect 内部
	}
};

// 把指定 edge/vertex 立即 snap 到鼠标位置，返回新 rect
export const snapRectToMousePosition = (
	dragMode: DragMode,
	rect: ElementRect,
	mousePosition: MousePosition,
): ElementRect => {
	const { mouseX, mouseY } = mousePosition;
	const next = { ...rect };
	switch (dragMode) {
		case DragMode.TopLeft:
			next.min_x = mouseX;
			next.min_y = mouseY;
			break;
		case DragMode.TopRight:
			next.max_x = mouseX;
			next.min_y = mouseY;
			break;
		case DragMode.BottomRight:
			next.max_x = mouseX;
			next.max_y = mouseY;
			break;
		case DragMode.BottomLeft:
			next.min_x = mouseX;
			next.max_y = mouseY;
			break;
		case DragMode.Top:
			next.min_y = mouseY;
			break;
		case DragMode.Right:
			next.max_x = mouseX;
			break;
		case DragMode.Bottom:
			next.max_y = mouseY;
			break;
		case DragMode.Left:
			next.min_x = mouseX;
			break;
		default:
			break;
	}
	// 翻转保护：snap 后保证 min <= max
	if (next.min_x > next.max_x) {
		[next.min_x, next.max_x] = [next.max_x, next.min_x];
	}
	if (next.min_y > next.max_y) {
		[next.min_y, next.max_y] = [next.max_y, next.min_y];
	}
	return next;
};

// 最小尺寸常量提升到函数外部
const MIN_RECT_SIZE = 1;

export const dragRect = (
	dragMode: DragMode,
	originRect: ElementRect,
	originMousePosition: MousePosition,
	mousePosition: MousePosition,
	lockWidthHeight: boolean = false,
	lockDragAspectRatio: number = 0,
): ElementRect => {
	// 计算鼠标移动的偏移量
	const deltaX = mousePosition.mouseX - originMousePosition.mouseX;
	const deltaY = mousePosition.mouseY - originMousePosition.mouseY;

	// 根据拖动模式更新相应的坐标
	let startMousePosition: MousePosition | undefined;
	let controlMousePosition: MousePosition | undefined;

	switch (dragMode) {
		case DragMode.All:
			startMousePosition = new MousePosition(
				originRect.min_x + deltaX,
				originRect.min_y + deltaY,
			);
			controlMousePosition = new MousePosition(
				originRect.max_x + deltaX,
				originRect.max_y + deltaY,
			);
			break;
		case DragMode.Top:
			startMousePosition = new MousePosition(
				originRect.min_x,
				originRect.max_y,
			);
			controlMousePosition = new MousePosition(
				originRect.max_x,
				originRect.min_y + deltaY,
			);
			break;
		case DragMode.Bottom:
			startMousePosition = new MousePosition(
				originRect.min_x,
				originRect.min_y,
			);
			controlMousePosition = new MousePosition(
				originRect.max_x,
				originRect.max_y + deltaY,
			);
			break;
		case DragMode.Left:
			startMousePosition = new MousePosition(
				originRect.max_x,
				originRect.min_y,
			);
			controlMousePosition = new MousePosition(
				originRect.min_x + deltaX,
				originRect.max_y,
			);
			break;
		case DragMode.Right:
			startMousePosition = new MousePosition(
				originRect.min_x,
				originRect.min_y,
			);
			controlMousePosition = new MousePosition(
				originRect.max_x + deltaX,
				originRect.max_y,
			);
			break;
		case DragMode.TopLeft:
			startMousePosition = new MousePosition(
				originRect.max_x,
				originRect.max_y,
			);
			controlMousePosition = new MousePosition(
				originRect.min_x + deltaX,
				originRect.min_y + deltaY,
			);
			break;
		case DragMode.TopRight:
			startMousePosition = new MousePosition(
				originRect.min_x,
				originRect.max_y,
			);
			controlMousePosition = new MousePosition(
				originRect.max_x + deltaX,
				originRect.min_y + deltaY,
			);
			break;
		case DragMode.BottomLeft:
			startMousePosition = new MousePosition(
				originRect.max_x,
				originRect.min_y,
			);
			controlMousePosition = new MousePosition(
				originRect.min_x + deltaX,
				originRect.max_y + deltaY,
			);
			break;
		case DragMode.BottomRight:
			startMousePosition = new MousePosition(
				originRect.min_x,
				originRect.min_y,
			);
			controlMousePosition = new MousePosition(
				originRect.max_x + deltaX,
				originRect.max_y + deltaY,
			);
			break;
	}

	// 移动整个选区时不应用宽高比锁定
	const shouldApplyAspectRatioLock = dragMode !== DragMode.All;

	return startMousePosition.toElementRect(
		controlMousePosition,
		lockWidthHeight && shouldApplyAspectRatioLock,
		lockDragAspectRatio > 0 && shouldApplyAspectRatioLock
			? lockDragAspectRatio
			: 0,
	);
};

export const limitRect = (
	currentRect: ElementRect,
	limitRect: ElementRect,
	checkMinRectSize: boolean = false,
) => {
	const { min_x, min_y, max_x, max_y } = currentRect;
	const {
		min_x: limitMinX,
		min_y: limitMinY,
		max_x: limitMaxX,
		max_y: limitMaxY,
	} = limitRect;

	const result: ElementRect = {
		min_x: Math.max(min_x, limitMinX),
		min_y: Math.max(min_y, limitMinY),
		max_x: Math.min(max_x, limitMaxX),
		max_y: Math.min(max_y, limitMaxY),
	};

	if (checkMinRectSize) {
		if (result.max_x - result.min_x < MIN_RECT_SIZE) {
			result.max_x = result.min_x + MIN_RECT_SIZE;
		}
		if (result.max_y - result.min_y < MIN_RECT_SIZE) {
			result.max_y = result.min_y + MIN_RECT_SIZE;
		}
	}

	return result;
};

export const positoinInRect = (
	rect: ElementRect,
	mousePosition: MousePosition,
) => {
	const { mouseX, mouseY } = mousePosition;

	return (
		mouseX >= rect.min_x &&
		mouseX <= rect.max_x &&
		mouseY >= rect.min_y &&
		mouseY <= rect.max_y
	);
};
