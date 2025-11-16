import { builtinChatModels, normalizeModelName } from "@/constants/chatModels";
import { withCache } from "@/utils/cache";
import { serviceFetch } from ".";

export interface ChatModel {
	model: string;
	name: string;
	thinking: boolean;
	support_vision: boolean;
}

export const getChatModels = async () => {
	return serviceFetch<ChatModel[]>("/api/v1/chat/models", {
		method: "GET",
	});
};

const mergeBuiltinModels = (list: ChatModel[] = []) => {
	const merged: ChatModel[] = list.map((item) => ({ ...item }));
	const existingModels = new Set(
		merged.map((item) => normalizeModelName(item.model)),
	);

	for (const model of builtinChatModels) {
		if (!existingModels.has(model.model)) {
			merged.push({ ...model });
		}
	}

	return merged;
};

// 内部函数：获取聊天模型数据
const fetchChatModels = async (): Promise<ChatModel[] | undefined> => {
	const resp = await getChatModels();
	const data = resp.success();
	if (data) {
		return mergeBuiltinModels(data ?? []);
	}

	return mergeBuiltinModels([]);
};

export const getChatModelsWithCache = withCache(fetchChatModels, {
	key: "getChatModels",
	duration: 60 * 60 * 1000, // 缓存 1 小时
});
