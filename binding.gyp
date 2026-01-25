{
	"targets": [
		{
			"target_name": "player",
			"sources": [
				"player/main.c"
			],
			"include_dirs": [
				"deps/ffmpeg/include"
			],
			"libraries": [
				"<(module_root_dir)/deps/ffmpeg/lib/avcodec.lib",
				"<(module_root_dir)/deps/ffmpeg/lib/avformat.lib",
				"<(module_root_dir)/deps/ffmpeg/lib/avutil.lib",
				"<(module_root_dir)/deps/ffmpeg/lib/swresample.lib"
			]
		}
	]
}
