# Maintain Snow Shot's public documentation

The repository README and user guides are the primary product documentation.
The official website introduces the product, selects downloads, and links back to
these guides. Keep technical build instructions in the existing platform documents.

## Product identity

- Product: **Snow Shot**; mention **SnowShot** naturally once as a common spelling.
- Official source: `https://github.com/mg-chao/snow-apps`.
- Official website: `https://snowshot.top/` (English), `/zh/` (Simplified Chinese).
- Explain that Snow Apps also contains Snow Image Viewer; do not rename the
  repository just to add keywords or break the old `mg-chao/snow-shot` redirect.
- Keep the repository About text descriptive in English and Chinese. Include the
  product category, supported systems and actual capabilities.
- Use accurate Topics such as `screenshot`, `screenshot-tool`, `screen-capture`,
  `scrolling-screenshot`, `ocr`, `image-annotation`, `screen-recorder`, `windows`,
  `macos`, `image-viewer` and `productivity`.

## When capabilities or packages change

1. Update both `README.md` and `README.zh-CN.md`, plus the affected guide in English
   and `zh-CN/`. Update existing pages instead of creating near-duplicate articles.
2. Check actual release assets and package-manager manifests. Avoid hard-coding a
   release version in general download links; use `/releases/latest`.
3. Confirm OS/architecture support and full/Mini differences from the shipped
   product. Do not describe source availability as Linux support.
4. Distinguish local OCR from initial model downloads, online translation and AI
   providers. Keep privacy statements aligned with the published policy.
5. Check relative links and GitHub-rendered headings. Retain headings that are
   already linked externally, or update their inbound links.
6. Update the website's matching introduction and guide links when needed.

## Measuring discoverability

Maintain a dated baseline for the same search engine, language and region. Track
brand queries (`Snow Shot`, `SnowShot`) separately from intent queries such as
`开源截图工具`, `离线 OCR 截图`, `滚动截图 开源` and `open source screenshot tool`.
Record organic results and AI-summary sources separately. Search results vary;
one session is not a global rank or proof of a penalty.

Use repository traffic/referrer data where available and Bing Webmaster Tools for
the domain you control. Compare impressions, clicks, crawl errors, indexed URLs
and chosen canonicals over multiple weeks. GitHub Topics aid repository discovery;
stars and domain suffixes are not standalone measures of search-engine authority.

The website can submit its own changed URLs through IndexNow. Do not submit
`github.com` URLs using the website's verification key: the verified hosts differ.
GitHub controls its own crawling and indexing configuration.

For relevant directories or community introductions, use an accurate description
and link to this official repository. Correct outdated references through the
normal contribution process when authorized. Do not buy links, publish duplicate
keyword pages, manufacture endorsements, or label third-party sites as malicious
without evidence.

## Official references

- [GitHub repository search](https://docs.github.com/en/search-github/searching-on-github/searching-for-repositories)
- [GitHub Topics](https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/customizing-your-repository/classifying-your-repository-with-topics)
- [Bing Webmaster Guidelines](https://www.bing.com/webmasters/help/bing-webmaster-guidelines-30fba23a)
