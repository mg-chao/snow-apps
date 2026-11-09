# Offline screenshot OCR with Snow Shot

[All guides](README.md) · [简体中文](zh-CN/offline-ocr.md)

Snow Shot can recognize text in a screenshot locally, so you can copy an error
message, a paragraph or another piece of text that cannot be selected in its
original application. Standard OCR does not require a cloud OCR service once
the necessary model resources are installed.

## Prepare for offline use

| Package | Preparation |
| --- | --- |
| Full Windows offline installer | Includes the default OCR resources; verify your target language on a sample |
| Full online installer | Complete the component/model downloads before going offline |
| Mini | Enable manual text recognition in settings and complete its first-use download |

Do not assume a smaller installer contains every recognition resource. If you
need an air-gapped installation, prepare and test the full Windows offline package
before moving it into that environment.

## Extract text from a screenshot

1. Press **F1** and select the text you want to recognize.
2. Choose **Text recognition** in the capture toolbar.
3. Review the recognized text, correct any mistakes, and copy or export the result.
4. For repeated use, assign **Text recognition** or **Copy after text recognition**
   a global shortcut. The latter copies the result and ends the screenshot session.

Prefer a tight selection around the text, with the application at a readable
scale. Recognition of tiny, blurred, stylized or low-contrast text can be less
reliable. Check numbers, punctuation and similar-looking characters before reusing
the output, particularly in commands, account numbers or tabular data.

## Text, tables, QR codes and translation

- **Text recognition:** available in the full edition and, after setup, Mini.
- **Table and QR recognition:** use the full edition; Mini omits these tools.
- **Translation:** has separate provider and network requirements. Local OCR
  does not make an online translation provider work offline.
- **Optional AI features:** their processing and data handling depend on the
  chosen provider or connected application.

Refer to the [privacy policy](https://snowshot.top/privacy.html) for how local
content, online features and connections are handled.

## Troubleshooting

If recognition requests a download, complete it while connected, then test again
offline. If a feature is missing, check whether you installed Mini. If a result is
wrong, retry with a larger text size and a simpler selection before reporting it.
Include the app version, edition, language and a non-sensitive sample in
[an issue](https://github.com/mg-chao/snow-apps/issues).

[Scrolling screenshots](scrolling-screenshots.md) · [Getting started](getting-started.md)
