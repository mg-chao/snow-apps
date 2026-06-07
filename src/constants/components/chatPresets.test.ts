import { describe, it, expect } from "vitest";
import { chatApiPresets, type ChatApiPreset } from "./chatPresets";

describe("chatApiPresets", () => {
	it("should export a non-empty array of presets", () => {
		expect(Array.isArray(chatApiPresets)).toBe(true);
		expect(chatApiPresets.length).toBeGreaterThan(0);
	});

	it("each preset should have all required fields", () => {
		const requiredFields: (keyof ChatApiPreset)[] = [
			"id",
			"labelKey",
			"api_uri",
			"api_model",
			"model_name",
			"support_thinking",
			"support_vision",
		];

		for (const preset of chatApiPresets) {
			for (const field of requiredFields) {
				expect(preset).toHaveProperty(field);
			}
		}
	});

	it("each preset should have a unique id", () => {
		const ids = chatApiPresets.map((p) => p.id);
		expect(new Set(ids).size).toBe(ids.length);
	});

	it("each preset should not contain api_key", () => {
		for (const preset of chatApiPresets) {
			expect(preset).not.toHaveProperty("api_key");
		}
	});

	it("MiniMax presets should use the correct API base URL", () => {
		const minimaxPresets = chatApiPresets.filter((p) =>
			p.id.startsWith("minimax-"),
		);
		expect(minimaxPresets.length).toBeGreaterThan(0);

		for (const preset of minimaxPresets) {
			expect(preset.api_uri).toBe("https://api.minimax.io/v1/");
		}
	});

	it("should contain MiniMax M3 preset as the default (first) entry with vision and thinking support", () => {
		const m3 = chatApiPresets.find((p) => p.id === "minimax-m3");
		expect(m3).toBeDefined();
		expect(m3!.api_model).toBe("MiniMax-M3");
		expect(m3!.support_thinking).toBe(true);
		expect(m3!.support_vision).toBe(true);
		// M3 should be first in the list (the default preset)
		expect(chatApiPresets[0].id).toBe("minimax-m3");
	});

	it("should contain MiniMax M2.7 preset with thinking support", () => {
		const m27 = chatApiPresets.find((p) => p.id === "minimax-m2.7");
		expect(m27).toBeDefined();
		expect(m27!.api_model).toBe("MiniMax-M2.7");
		expect(m27!.support_thinking).toBe(true);
		expect(m27!.support_vision).toBe(false);
	});

	it("should contain MiniMax M2.7 Highspeed preset without thinking", () => {
		const m27hs = chatApiPresets.find(
			(p) => p.id === "minimax-m2.7-highspeed",
		);
		expect(m27hs).toBeDefined();
		expect(m27hs!.api_model).toBe("MiniMax-M2.7-highspeed");
		expect(m27hs!.support_thinking).toBe(false);
		expect(m27hs!.support_vision).toBe(false);
	});

	it("each preset labelKey should follow the i18n key convention", () => {
		for (const preset of chatApiPresets) {
			expect(preset.labelKey).toMatch(
				/^settings\.functionSettings\.chatSettings\.preset\./,
			);
		}
	});
});
