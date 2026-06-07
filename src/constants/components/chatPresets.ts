import type { ChatApiConfig } from "@/types/appSettings";

export type ChatApiPreset = Omit<ChatApiConfig, "api_key"> & {
	/** Preset identifier */
	id: string;
	/** i18n message key for the preset label */
	labelKey: string;
};

/**
 * Pre-configured LLM provider presets.
 * Users only need to fill in their API key to get started.
 */
export const chatApiPresets: ChatApiPreset[] = [
	{
		id: "minimax-m3",
		labelKey: "settings.functionSettings.chatSettings.preset.minimax.m3",
		api_uri: "https://api.minimax.io/v1/",
		api_model: "MiniMax-M3",
		model_name: "MiniMax M3",
		support_thinking: true,
		support_vision: true,
	},
	{
		id: "minimax-m2.7",
		labelKey: "settings.functionSettings.chatSettings.preset.minimax.m2_7",
		api_uri: "https://api.minimax.io/v1/",
		api_model: "MiniMax-M2.7",
		model_name: "MiniMax M2.7",
		support_thinking: true,
		support_vision: false,
	},
	{
		id: "minimax-m2.7-highspeed",
		labelKey:
			"settings.functionSettings.chatSettings.preset.minimax.m2_7_highspeed",
		api_uri: "https://api.minimax.io/v1/",
		api_model: "MiniMax-M2.7-highspeed",
		model_name: "MiniMax M2.7 Highspeed",
		support_thinking: false,
		support_vision: false,
	},
];
