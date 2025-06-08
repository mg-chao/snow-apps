'use client';

import React, { useEffect } from 'react';
import { useCallback, useImperativeHandle, useMemo, useRef } from 'react';
import { Excalidraw } from '@mg-chao/excalidraw';
import {
    ExcalidrawInitialDataState,
    ExcalidrawImperativeAPI,
    ExcalidrawActionType,
    ExcalidrawPropsCustomOptions,
    AppState,
    ExcalidrawProps,
} from '@mg-chao/excalidraw/types';
import '@mg-chao/excalidraw/index.css';
import { useStateSubscriber } from '@/hooks/useStateSubscriber';
import { withStatePublisher } from '@/hooks/useStatePublisher';
import { ExcalidrawKeyEventHandler } from './components/excalidrawKeyEventHandler';
import {
    convertLocalToLocalCode,
    DrawCoreActionType,
    DrawState,
    DrawStatePublisher,
    ExcalidrawEventCallbackPublisher,
    ExcalidrawEventCallbackType,
    ExcalidrawEventPublisher,
    ExcalidrawKeyEventPublisher,
    ExcalidrawOnHandleEraserPublisher,
} from './extra';
import { useIntl } from 'react-intl';
import { theme } from 'antd';
import { layoutRenders } from './excalidrawRenders';
import { pickerRenders } from './excalidrawRenders';
import { ElementRect } from '@/commands';
import { ExcalidrawAppStateStore } from '@/utils/appStore';
import { debounce } from 'es-toolkit';
import { useHistory } from './components/historyContext';
import { SerialNumberTool } from '@/app/fullScreenDraw/components/drawCore/components/serialNumberTool';

const strokeWidthList = [1, 2, 4];
const fontSizeList = [16, 20, 28, 36];

// 在 DrawCacheLayerCore 组件外部添加一个辅助函数
const getNextValueInList = <T,>(currentValue: T, valueList: T[], isIncrease: boolean): T => {
    const currentIndex = valueList.indexOf(currentValue);
    if (currentIndex !== -1) {
        if (isIncrease) {
            // 选择下一个值（循环到开头）
            const nextIndex = (currentIndex + 1) % valueList.length;
            return valueList[nextIndex];
        } else {
            // 选择上一个值（循环到结尾）
            const prevIndex = (currentIndex - 1 + valueList.length) % valueList.length;
            return valueList[prevIndex];
        }
    } else {
        // 如果当前值不在列表中
        return isIncrease ? valueList[0] : valueList[valueList.length - 1];
    }
};

const storageKey = 'global';
const DrawCoreComponent: React.FC<{
    actionRef: React.RefObject<DrawCoreActionType | undefined>;
    zIndex: number;
    layoutMenuZIndex: number;
    onLoad?: () => void;
}> = ({ actionRef, zIndex, layoutMenuZIndex, onLoad }) => {
    const { token } = theme.useToken();
    const intl = useIntl();

    const { history } = useHistory();

    const initialData = useMemo<ExcalidrawInitialDataState>(() => {
        return {
            appState: { viewBackgroundColor: '#00000000' },
        };
    }, []);

    const [getDrawState] = useStateSubscriber(DrawStatePublisher, undefined);
    const [, setExcalidrawEvent] = useStateSubscriber(ExcalidrawEventPublisher, undefined);
    const [, setExcalidrawOnHandleEraserEvent] = useStateSubscriber(
        ExcalidrawOnHandleEraserPublisher,
        undefined,
    );
    const [getExcalidrawKeyEvent] = useStateSubscriber(ExcalidrawKeyEventPublisher, undefined);
    const [, setExcalidrawEventCallback] = useStateSubscriber(
        ExcalidrawEventCallbackPublisher,
        undefined,
    );
    const drawCacheLayerElementRef = useRef<HTMLDivElement>(null);
    const excalidrawAPIRef = useRef<ExcalidrawImperativeAPI>(undefined);
    const excalidrawAPI = useCallback((api: ExcalidrawImperativeAPI) => {
        excalidrawAPIRef.current = api;
    }, []);
    const excalidrawActionRef = useRef<ExcalidrawActionType>(undefined);

    const updateScene = useCallback<DrawCoreActionType['updateScene']>((...args) => {
        excalidrawAPIRef.current?.updateScene(...args);
    }, []);

    const enableRef = useRef<boolean>(false);
    const setEnable = useCallback<DrawCoreActionType['setEnable']>((enable: boolean) => {
        if (!drawCacheLayerElementRef.current) {
            return;
        }

        enableRef.current = enable;
        if (enable) {
            drawCacheLayerElementRef.current.style.pointerEvents = 'auto';
            drawCacheLayerElementRef.current.style.opacity = '1';
        } else {
            drawCacheLayerElementRef.current.style.pointerEvents = 'none';
        }
    }, []);

    const getCanvas = useCallback<DrawCoreActionType['getCanvas']>(() => {
        const canvas = document.getElementById(
            'excalidraw__content-canvas',
        ) as HTMLCanvasElement | null;
        return canvas;
    }, []);

    const getCanvasContext = useCallback<DrawCoreActionType['getCanvasContext']>(() => {
        const canvas = getCanvas();
        if (!canvas) {
            return;
        }

        return canvas.getContext('2d');
    }, [getCanvas]);

    const getImageData = useCallback<DrawCoreActionType['getImageData']>(
        async (selectRect: ElementRect) => {
            const canvasContext = getCanvasContext();
            if (!canvasContext) {
                return;
            }

            return canvasContext.getImageData(
                selectRect.min_x,
                selectRect.min_y,
                selectRect.max_x - selectRect.min_x,
                selectRect.max_y - selectRect.min_y,
            );
        },
        [getCanvasContext],
    );

    const excalidrawAppStateStoreRef = useRef<ExcalidrawAppStateStore>(undefined);
    const excalidrawAppStateStoreValue = useRef<
        | {
            appState: Partial<AppState>;
        }
        | undefined
    >(undefined);
    useEffect(() => {
        if (excalidrawAppStateStoreRef.current) {
            return;
        }

        excalidrawAppStateStoreRef.current = new ExcalidrawAppStateStore();
        excalidrawAppStateStoreRef.current.init().then(() => {
            excalidrawAppStateStoreRef.current!.get(storageKey).then((value) => {
                if (value) {
                    if (excalidrawAPIRef.current) {
                        // 未初始化 setstate 报错，未发现具体原因，延迟处理下
                        setTimeout(() => {
                            excalidrawAPIRef.current!.updateScene({
                                appState: {
                                    ...(value.appState as AppState),
                                    viewBackgroundColor: '#00000000',
                                },
                            });
                        }, 0);
                    } else {
                        excalidrawAppStateStoreValue.current = {
                            appState: {
                                ...(value.appState as AppState),
                                viewBackgroundColor: '#00000000',
                            },
                        };
                    }
                }
            });
        });
    }, []);

    const handleWheel = useCallback(
        (
            event: WheelEvent | React.WheelEvent<HTMLDivElement | HTMLCanvasElement>,
            zoomAction?: () => void,
        ) => {
            if (!enableRef.current) {
                return;
            }

            if (!excalidrawAPIRef.current) {
                return;
            }

            if ((event.metaKey || event.ctrlKey) && zoomAction) {
                zoomAction();
                return;
            }

            const appState = excalidrawAPIRef.current.getAppState();
            if (!appState) {
                return;
            }

            const isIncrease = event.deltaY < 0;

            if (getDrawState() === DrawState.Blur) {
                const currentBlur = appState.currentItemBlur;
                const targetBlur = Math.max(
                    Math.min(currentBlur + (isIncrease ? 1 : -1) * 10, 100),
                    0,
                );

                excalidrawAPIRef.current?.updateScene({
                    appState: {
                        ...appState,
                        currentItemBlur: targetBlur,
                    },
                });
                return;
            } else if (
                getDrawState() === DrawState.Text ||
                getDrawState() === DrawState.SerialNumber
            ) {
                const currentFontSize = appState.currentItemFontSize;

                const targetFontSize = getNextValueInList(
                    currentFontSize,
                    fontSizeList,
                    isIncrease,
                );

                setExcalidrawEventCallback({
                    event: ExcalidrawEventCallbackType.ChangeFontSize,
                    params: {
                        fontSize: targetFontSize,
                    },
                });
                setExcalidrawEventCallback(undefined);

                return;
            } else {
                const currentStrokeWidth = appState.currentItemStrokeWidth;
                const targetStrokeWidth = getNextValueInList(
                    currentStrokeWidth,
                    strokeWidthList,
                    isIncrease,
                );

                excalidrawAPIRef.current?.updateScene({
                    appState: {
                        ...appState,
                        currentItemStrokeWidth: targetStrokeWidth,
                    },
                });

                return;
            }
        },
        [getDrawState, setExcalidrawEventCallback],
    );

    useImperativeHandle(
        actionRef,
        () => ({
            setActiveTool: (...args) => {
                excalidrawAPIRef.current?.setActiveTool(...args);
            },
            syncActionResult: (...args) => {
                excalidrawActionRef.current?.syncActionResult(...args);
            },
            updateScene,
            setEnable,
            getAppState: () => {
                return excalidrawAPIRef.current?.getAppState();
            },
            getImageData,
            getCanvasContext,
            getCanvas,
            getDrawCacheLayerElement: () => drawCacheLayerElementRef.current,
            getExcalidrawAPI: () => excalidrawAPIRef.current,
            handleWheel: (ev: WheelEvent | React.WheelEvent<HTMLDivElement>) => {
                handleWheel(ev);
            },
        }),
        [getCanvas, getCanvasContext, getImageData, handleWheel, setEnable, updateScene],
    );

    const shouldResizeFromCenter = useCallback<
        NonNullable<ExcalidrawPropsCustomOptions['shouldResizeFromCenter']>
    >(() => {
        return getExcalidrawKeyEvent().resizeFromCenter;
    }, [getExcalidrawKeyEvent]);

    const shouldMaintainAspectRatio = useCallback<
        NonNullable<ExcalidrawPropsCustomOptions['shouldMaintainAspectRatio']>
    >(() => {
        return getExcalidrawKeyEvent().maintainAspectRatio;
    }, [getExcalidrawKeyEvent]);

    const shouldRotateWithDiscreteAngle = useCallback<
        NonNullable<ExcalidrawPropsCustomOptions['shouldRotateWithDiscreteAngle']>
    >(() => {
        return getExcalidrawKeyEvent().rotateWithDiscreteAngle;
    }, [getExcalidrawKeyEvent]);

    const shouldSnapping = useCallback<
        NonNullable<ExcalidrawPropsCustomOptions['shouldSnapping']>
    >(() => {
        return getExcalidrawKeyEvent().autoAlign;
    }, [getExcalidrawKeyEvent]);

    const onHistoryChange = useCallback<
        NonNullable<ExcalidrawPropsCustomOptions['onHistoryChange']>
    >(
        (_, type) => {
            if (type === 'record') {
                history.pushDrawCacheRecordAction(excalidrawActionRef);
            }
        },
        [history],
    );

    const saveAppState = useCallback(async () => {
        const appState = excalidrawAPIRef.current?.getAppState();
        if (!appState) {
            return;
        }

        const storageAppState: Partial<AppState> = {};
        Object.keys(appState)
            .filter((item) => item.startsWith('currentItem'))
            .forEach((item) => {
                const value = appState[item as keyof AppState];
                if (!value) {
                    return;
                }

                storageAppState[item as keyof AppState] = value;
            });

        await excalidrawAppStateStoreRef.current!.set(storageKey, {
            appState: storageAppState,
        });
    }, []);
    const saveAppStateDebounce = useMemo(() => debounce(saveAppState, 1000), [saveAppState]);

    const getExtraTools = useCallback<
        NonNullable<ExcalidrawPropsCustomOptions['getExtraTools']>
    >(() => {
        if (getDrawState() === DrawState.SerialNumber) {
            return ['serialNumber'];
        }

        return [];
    }, [getDrawState]);

    const onPointerDown = useCallback<NonNullable<ExcalidrawProps['onPointerDown']>>(
        (activeTool, pointerDownState) => {
            setExcalidrawEvent({
                event: 'onPointerDown',
                params: {
                    activeTool,
                    pointerDownState,
                },
            });
            setExcalidrawEvent(undefined);
        },
        [setExcalidrawEvent],
    );

    const excalidrawAPICallback = useCallback<NonNullable<ExcalidrawProps['excalidrawAPI']>>(
        (api) => {
            excalidrawAPI(api);
            onLoad?.();
            if (excalidrawAppStateStoreValue.current) {
                // 未初始化 setstate 报错，未发现具体原因，延迟处理下
                setTimeout(() => {
                    if (!excalidrawAppStateStoreValue.current) {
                        return;
                    }

                    excalidrawAPIRef.current?.updateScene({
                        appState: {
                            ...(excalidrawAppStateStoreValue.current!.appState as AppState),
                        },
                    });
                }, 0);
            }
        },
        [excalidrawAPI, onLoad],
    );

    const excalidrawOnChange = useCallback<NonNullable<ExcalidrawProps['onChange']>>(
        (elements, appState, files) => {
            saveAppStateDebounce();
            setExcalidrawEvent({
                event: 'onChange',
                params: {
                    elements,
                    appState,
                    files,
                },
            });
            setExcalidrawEvent(undefined);
        },
        [saveAppStateDebounce, setExcalidrawEvent],
    );

    const excalidrawCustomOptions = useMemo<NonNullable<ExcalidrawPropsCustomOptions>>(() => {
        return {
            disableKeyEvents: true,
            hideFooter: true,
            onWheel: handleWheel,
            hideMainToolbar: true,
            hideContextMenu: true,
            shouldResizeFromCenter,
            shouldMaintainAspectRatio,
            shouldSnapping,
            getExtraTools,
            shouldRotateWithDiscreteAngle,
            pickerRenders: pickerRenders,
            layoutRenders: layoutRenders,
            onHistoryChange,
            onHandleEraser: (elements) => {
                setExcalidrawOnHandleEraserEvent({
                    elements,
                });
            },
        };
    }, [
        getExtraTools,
        handleWheel,
        onHistoryChange,
        setExcalidrawOnHandleEraserEvent,
        shouldMaintainAspectRatio,
        shouldResizeFromCenter,
        shouldRotateWithDiscreteAngle,
        shouldSnapping,
    ]);

    const excalidrawLangCode = useMemo(() => convertLocalToLocalCode(intl.locale), [intl.locale]);

    return (
        <>
            <div ref={drawCacheLayerElementRef} className="draw-core-layer">
                <Excalidraw
                    actionRef={excalidrawActionRef}
                    initialData={initialData}
                    handleKeyboardGlobally
                    onPointerDown={onPointerDown}
                    excalidrawAPI={excalidrawAPICallback}
                    customOptions={excalidrawCustomOptions}
                    onChange={excalidrawOnChange}
                    langCode={excalidrawLangCode}
                />
                <ExcalidrawKeyEventHandler />

                <SerialNumberTool />

                <style jsx>{`
                    .draw-core-layer {
                        top: 0;
                        left: 0;
                        width: 100%;
                        height: 100%;
                        opacity: 0;
                    }

                    .draw-core-layer :global(.excalidraw .layer-ui__wrapper) {
                        z-index: unset !important;
                    }

                    .draw-core-layer :global(.excalidraw .layout-menu-render) {
                        --popup-bg-color: ${token.colorBgContainer};
                    }

                    .draw-core-layer :global(.excalidraw .layout-menu-render .picker) {
                        box-shadow: 0 0 3px 0px ${token.colorInfoHover};
                    }

                    .draw-core-layer :global(.excalidraw .layout-menu-render) {
                        position: fixed;
                        z-index: ${layoutMenuZIndex};
                        left: 0;
                        top: 0;
                        box-sizing: border-box;
                        background-color: ${token.colorBgContainer};
                        transition: opacity ${token.motionDurationFast} ${token.motionEaseInOut};
                        box-shadow: 0 0 3px 0px ${token.colorPrimaryHover};
                        color: ${token.colorText};
                        border-radius: ${token.borderRadiusLG}px;
                        animation: slideIn ${token.motionDurationFast} ${token.motionEaseInOut};
                    }

                    @keyframes slideIn {
                        from {
                            opacity: 0;
                        }
                        to {
                            opacity: 1;
                        }
                    }

                    .draw-core-layer :global(.layout-menu-render-drag-button) {
                        text-align: center;
                        margin-top: ${token.marginXS}px;
                        margin-bottom: -${token.marginXS}px;
                    }

                    .draw-core-layer :global(.layout-menu-render-drag-button > span) {
                        transform: rotate(90deg);
                    }

                    .draw-core-layer :global(.Island.App-menu__left) {
                        --text-primary-color: ${token.colorText};

                        background-color: unset !important;
                        box-shadow: unset !important;
                        position: relative !important;
                        padding: ${token.paddingSM}px ${token.paddingSM}px !important;
                    }

                    .draw-core-layer :global(.excalidraw-container-inner) {
                        z-index: ${zIndex};
                        position: fixed;
                    }

                    .draw-core-layer :global(.excalidraw .radio-button-icon) {
                        width: var(--default-icon-size);
                        height: 100%;
                        display: flex;
                        align-items: center;
                    }

                    .draw-core-layer :global(.excalidraw .ant-radio-button-wrapper) {
                        padding-inline: ${token.paddingXS}px;
                    }

                    .draw-core-layer :global(.drag-button) {
                        color: ${token.colorTextQuaternary};
                        cursor: move;
                    }

                    .draw-core-layer :global(.draw-toolbar-drag) {
                        font-size: 18px;
                        margin-right: -3px;
                        margin-left: -3px;
                    }

                    .draw-core-layer :global(.excalidraw .scroll-back-to-content) {
                        display: none;
                    }
                `}</style>
            </div>
        </>
    );
};

export const DrawCore = React.memo(
    withStatePublisher(
        DrawCoreComponent,
        ExcalidrawKeyEventPublisher,
        ExcalidrawEventCallbackPublisher,
    ),
);
