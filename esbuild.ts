import { build } from "esbuild";

await build({
	entryPoints: ["src/index.ts"],
	bundle: true,
	charset: "utf8",
	format: "esm",
	minify: true,
	outdir: "dist",
	packages: "external",
	platform: "node",
	sourcemap: true,
	target: "node24",
	treeShaking: true,
});
