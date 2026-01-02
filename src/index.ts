import {
	AudioPlayerStatus,
	createAudioPlayer,
	createAudioResource,
	entersState,
	joinVoiceChannel,
	NoSubscriberBehavior,
	StreamType,
	VoiceConnectionStatus,
	type AudioPlayer,
	type AudioPlayerState,
} from "@discordjs/voice";
import {
	ActivityType,
	Client,
	Collection,
	ComponentType,
	Events,
	GatewayIntentBits,
	LimitedCollection,
	MessageFlags,
	Partials,
	Routes,
} from "discord.js";
import { decode } from "html-entities";
import { ok } from "node:assert/strict";
import { spawn, type ChildProcessByStdio } from "node:child_process";
import { env, exit, loadEnvFile } from "node:process";
import type { Readable } from "node:stream";

loadEnvFile();
let child: ChildProcessByStdio<null, Readable, null> | undefined;
const attachRecorder = <T extends AudioPlayer>(player: T): T => {
	if (child) return cleanPlayer(player);
	child = spawn(
		"ffmpeg",
		[
			// "-loglevel",
			// "warning",
			"-hide_banner",
			"-nostats",
			"-i",
			"https://icstream.rds.radio/rds",
			"-analyzeduration",
			"0",
			"-acodec",
			"libopus",
			"-f",
			"opus",
			"-ar",
			"48000",
			"-ac",
			"2",
			"-b:a",
			"256k",
			"-map_metadata",
			"-1",
			"pipe:1",
		],
		{ stdio: ["ignore", "pipe", "inherit"] },
	);
	player.play(
		createAudioResource(child.stdout, { inputType: StreamType.OggOpus }),
	);
	console.log("Recorder attached!");
	return player;
};
const cleanPlayer = <T extends AudioPlayer>(player: T): T => {
	if (!child) return player;
	console.log("Playback has stopped. Attempting to restart...");
	child?.kill();
	child = undefined;
	attachRecorder(player);
	return player;
};
const listener = ({}: AudioPlayerState, newState: AudioPlayerState) => {
	if (newState.status === AudioPlayerStatus.Idle) cleanPlayer(player);
};

const player = attachRecorder(
	createAudioPlayer({
		behaviors: { noSubscriber: NoSubscriberBehavior.Play },
	}),
).on("stateChange", listener);
const client = new Client({
	intents: [GatewayIntentBits.Guilds, GatewayIntentBits.GuildVoiceStates],
	allowedMentions: { parse: [] },
	partials: [
		Partials.Channel,
		Partials.GuildMember,
		Partials.GuildScheduledEvent,
		Partials.Message,
		Partials.Poll,
		Partials.PollAnswer,
		Partials.Reaction,
		Partials.SoundboardSound,
		Partials.ThreadMember,
		Partials.User,
	],
	makeCache: (manager) =>
		[
			"GuildManager",
			"ChannelManager",
			"GuildChannelManager",
			"RoleManager",
			"PermissionOverwriteManager",
		].includes(manager.name)
			? new Collection()
			: new LimitedCollection<string, any>({ maxSize: 0 }),
}).once(Events.ClientReady, async (client) => {
	const channel = client.channels.cache.get(env.CHANNEL_ID!)!;
	ok("guild" in channel);
	const connection = joinVoiceChannel({
		adapterCreator: channel.guild.voiceAdapterCreator,
		channelId: env.CHANNEL_ID!,
		guildId: channel.guildId,
	});

	process.prependOnceListener("SIGINT", () => {
		console.log("Stopping playback...");
		child?.kill("SIGINT");
		player.off("stateChange", listener);
		player.stop(true);
		console.log("Disconnecting...");
		connection.disconnect();
		connection.destroy();
	});
	connection.subscribe(player);
	await entersState(connection, VoiceConnectionStatus.Ready, 20_000);
	console.log("Ready!");
});
let id: string;

process.once("SIGINT", async () => {
	console.log("Destroying client...");
	await client.destroy();
	console.log("Exiting...");
	exit();
});
setInterval(async () => {
	try {
		if (!client.isReady()) return;
		const xml = await fetch("https://icstream.rds.radio/rds.xspf").then((res) =>
			res.text(),
		);
		const [, title, artist, year, newId] =
			xml.match(/<title>([^<]+)<\/title>/)?.[1]?.split("*") ?? [];

		if (newId === id) return;
		id = newId;
		const state = `${decode(title)}${artist ? ` - ${decode(artist)}` : ""}${
			year ? ` (${year})` : ""
		}`;
		client.user.presence.set({
			activities: [
				{
					name: "RDS",
					type: ActivityType.Listening,
					url: "https://rds.it",
					state,
				},
			],
		});
		const channel = client.channels.cache.get(env.CHANNEL_ID!)!;
		const url = new URL("https://cdnapi.rds.it/v2/site/get_song");
		url.search = `idsong=${id}`;
		const data = await fetch(url).then((res) => res.json());

		await ("lastMessageId" in channel && channel.lastMessageId
			? client.rest.patch.bind(
					client.rest,
					Routes.channelMessage(env.CHANNEL_ID!, channel.lastMessageId),
			  )
			: client.rest.post.bind(
					client.rest,
					Routes.channelMessages(env.CHANNEL_ID!),
			  ))({
			body: {
				flags: MessageFlags.IsComponentsV2,
				components: [
					{
						type: ComponentType.Section,
						components: [
							{
								type: ComponentType.TextDisplay,
								content: `# ${state}\n${
									data.data.lyrics !== "none" ? data.data.lyrics ?? "" : ""
								}`,
							},
						],
						accessory: {
							type: ComponentType.Thumbnail,
							media: {
								url: data.data.id
									? data.data.music_log_cover_full
									: "https://web.rds.it/m/i",
							},
						},
					},
				],
			},
		});
	} catch (error) {
		console.error(error);
	}
}, 5_000);
await client.login();
