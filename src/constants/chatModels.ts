export type BuiltinChatModel = {
	model: string;
	name: string;
	thinking: boolean;
	support_vision: boolean;
};

export const builtinChatModels: BuiltinChatModel[] = [
	{
		model: "gemini-2.5-flash",
		name: "Gemini 2.5 Flash",
		thinking: false,
		support_vision: true,
	},
	{
		model: "gemini-2.5-pro",
		name: "Gemini 2.5 Pro",
		thinking: true,
		support_vision: true,
	},
	{
		model: "gemini-2.0-flash",
		name: "Gemini 2.0 Flash",
		thinking: false,
		support_vision: true,
	},
	{
		model: "gemini-2.0-flash-thinking",
		name: "Gemini 2.0 Flash Thinking",
		thinking: true,
		support_vision: true,
	},
];

const GEMINI_MODEL_PREFIX = "models/";

export const normalizeModelName = (model: string) => {
	if (model.startsWith(GEMINI_MODEL_PREFIX)) {
		return model.slice(GEMINI_MODEL_PREFIX.length);
	}

	return model;
};

export const toGeminiModelPath = (model: string) => {
	if (model.startsWith(GEMINI_MODEL_PREFIX)) {
		return model;
	}

	return `${GEMINI_MODEL_PREFIX}${model}`;
};

export const isGeminiModel = (model?: string) => {
	if (!model) {
		return false;
	}

	const normalized = normalizeModelName(model);
	return builtinChatModels.some((item) => item.model === normalized);
};
