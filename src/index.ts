import { REST } from "@discordjs/rest";
import {
	entersState,
	joinVoiceChannel,
	VoiceConnectionStatus,
	type VoiceConnection,
} from "@discordjs/voice";
import {
	CompressionMethod,
	WebSocketManager,
	WebSocketShardEvents,
} from "@discordjs/ws";
import {
	ActivityType,
	APIVersion,
	ComponentType,
	GatewayDispatchEvents,
	GatewayIntentBits,
	GatewayOpcodes,
	MessageFlags,
	PresenceUpdateStatus,
	RouteBases,
	Routes,
	type GatewayDispatchPayload,
	type RESTGetAPIChannelMessagesResult,
	type RESTPatchAPIChannelMessageJSONBody,
	type RESTPostAPIChannelMessageJSONBody,
	type RESTPostAPIChannelMessageResult,
} from "discord-api-types/v10";
import { decode } from "html-entities";
import { spawn } from "node:child_process";
import { error, log, time, timeEnd } from "node:console";
import { once } from "node:events";
import { env, loadEnvFile } from "node:process";
import { pipeline } from "node:stream/promises";
import { opus } from "prism-media";

time("Ready");
loadEnvFile();
// TODO: use directly -f data with ffmpeg native binding
const child = spawn(
	"ffmpeg",
	[
		"-loglevel",
		"warning",
		"-hide_banner",
		"-nostats",
		"-i",
		"https://icstream.rds.radio/rds",
		"-analyzeduration",
		"0",
		"-map",
		"0:a:0",
		"-map_metadata",
		"-1",
		"-acodec",
		"libopus",
		"-f",
		"opus",
		"-ar",
		"48k",
		"-ac",
		"2",
		"-b:a",
		"256k",
		"pipe:1",
	],
	{ stdio: ["ignore", "pipe", "inherit"] },
);
const manager = new WebSocketManager({
	compression: CompressionMethod.ZlibNative,
	handshakeTimeout: 20_000,
	helloTimeout: 20_000,
	intents: GatewayIntentBits.GuildVoiceStates,
	largeThreshold: 50,
	readyTimeout: 20_000,
	rest: new REST({
		handlerSweepInterval: 0,
		hashSweepInterval: 0,
		version: APIVersion,
	}).setToken(env["DISCORD_TOKEN"]!),
	token: env["DISCORD_TOKEN"]!,
});
const stream = new opus.OggDemuxer().resume();
let connection: VoiceConnection;
let id: string;
let lastMessageId: string | undefined;
let timeout: NodeJS.Timeout;

pipeline(child.stdout, stream);
child.unref();
log("Connecting...");
manager.connect();
[
	[
		{
			user: { id },
		},
	],
	lastMessageId,
] = await Promise.all([
	once(manager, WebSocketShardEvents.Ready),
	fetch(
		RouteBases.api + Routes.channelMessages(env["CHANNEL_ID"]!) + "?limit=1",
		{
			headers: { Authorization: `Bot ${env["DISCORD_TOKEN"]!}` },
		},
	)
		.then((res) => res.json())
		.then((value) => (value as RESTGetAPIChannelMessagesResult)[0]?.id),
]);
connection = joinVoiceChannel({
	adapterCreator: (methods) => {
		const listener = (payload: GatewayDispatchPayload) => {
			if (payload.t === GatewayDispatchEvents.VoiceServerUpdate)
				methods.onVoiceServerUpdate(payload.d);
			else if (
				payload.t === GatewayDispatchEvents.VoiceStateUpdate &&
				payload.d.user_id === id &&
				payload.d.guild_id === env["GUILD_ID"]!
			)
				methods.onVoiceStateUpdate(payload.d);
		};

		manager.on(WebSocketShardEvents.Dispatch, listener);
		return {
			sendPayload: (payload) => {
				manager.send(0, payload);
				return true;
			},
			destroy: () => manager.off(WebSocketShardEvents.Dispatch, listener),
		};
	},
	channelId: env["CHANNEL_ID"]!,
	guildId: env["GUILD_ID"]!,
});
log("Joining voice channel...");
[lastMessageId] = await Promise.all([
	lastMessageId ??
		fetch(RouteBases.api + Routes.channelMessages(env["CHANNEL_ID"]!), {
			body: JSON.stringify({
				flags: MessageFlags.IsComponentsV2,
				components: [{ type: ComponentType.TextDisplay, content: "_ _" }],
			} satisfies RESTPostAPIChannelMessageJSONBody),
			method: "POST",
			headers: {
				"Authorization": `Bot ${env["DISCORD_TOKEN"]!}`,
				"Content-Type": "application/json",
			},
		})
			.then((res) => res.json())
			.then((res) => (res as RESTPostAPIChannelMessageResult).id),
	entersState(
		connection,
		VoiceConnectionStatus.Ready,
		AbortSignal.timeout(20_000),
	),
]);
stream.on("data", (packet) => {
	const { state } = connection;
	if (state.status !== VoiceConnectionStatus.Ready) return;
	const { networking } = state;

	networking.prepareAudioPacket(packet);
	networking.dispatchAudio();
});
timeout = setInterval<[{ id: string | undefined }]>(
	async (args) => {
		try {
			let res = await fetch("https://icstream.rds.radio/rds.xspf");
			if (!res.ok)
				throw new Error(`Fetch failed with ${res.status} ${res.statusText}`);
			const xml = await res.text();
			const [, title, artist, year, newId] =
				xml.match(/<title>([^<]+)<\/title>/)?.[1]?.split("*") ?? [];
			if (newId === args.id) return;
			args.id = newId;
			const state = `${decode(title)}${artist ? ` - ${decode(artist)}` : ""}${
				year ? ` (${year})` : ""
			}`;

			[res] = await Promise.all([
				fetch(`https://cdnapi.rds.it/v2/site/get_song?idsong=${id}`),
				manager.send(0, {
					op: GatewayOpcodes.PresenceUpdate,
					d: {
						activities: [
							{
								name: "RDS",
								type: ActivityType.Listening,
								url: "https://rds.it",
								state,
							},
						],
						afk: false,
						since: null,
						status: PresenceUpdateStatus.Online,
					},
				}),
			]);
			if (!res.ok)
				throw new Error(`Fetch failed with ${res.status} ${res.statusText}`);
			const data = (await res.json()) as {
				data: { lyrics?: string; id: number; music_log_cover_full: string };
			};
			res = await fetch(
				RouteBases.api +
					Routes.channelMessage(env["CHANNEL_ID"]!, lastMessageId),
				{
					method: "PATCH",
					headers: {
						"Authorization": `Bot ${env["DISCORD_TOKEN"]!}`,
						"Content-Type": "application/json",
					},
					body: JSON.stringify({
						flags: MessageFlags.IsComponentsV2,
						allowed_mentions: { parse: [] },
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
					} satisfies RESTPatchAPIChannelMessageJSONBody),
				},
			);
			if (!res.ok)
				throw new Error(`Fetch failed with ${res.status} ${res.statusText}`);
		} catch (err) {
			error(err);
		}
	},
	5_000,
	{ id: undefined },
).unref();
process.once("SIGINT", () => {
	log("Exiting...");
	clearInterval(timeout);
	child.kill("SIGINT");
	connection.disconnect();
	connection.destroy();
	manager.destroy();
});
timeEnd("Ready");
