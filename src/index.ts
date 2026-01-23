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
	APIVersion,
	ComponentType,
	GatewayDispatchEvents,
	GatewayIntentBits,
	MessageFlags,
	Routes,
	type GatewayDispatchPayload,
	type RESTGetAPIChannelMessagesResult,
	type RESTPostAPIChannelMessageJSONBody,
	type RESTPostAPIChannelMessageResult,
} from "discord-api-types/v10";
import { log, time, timeEnd } from "node:console";
import { once } from "node:events";
import { createRequire } from "node:module";
import { env, loadEnvFile } from "node:process";
import { Client } from "undici";
const { AudioPlayer } = createRequire(import.meta.url)("./play.node");

time("Ready");
loadEnvFile();
const agent = new Client("https://discord.com", {
	allowH2: true,
	headersTimeout: 20_000,
});
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
		agent,
	}).setToken(env["DISCORD_TOKEN"]!),
	token: env["DISCORD_TOKEN"]!,
});
let connection: VoiceConnection;
let id: string;
let lastMessageId: string | undefined;
let player: {
	play(url: string): void;
	stop(timeout?: number, force?: boolean): void;
	destroy(timeout?: number, force?: boolean): void;
};
// let timeout: NodeJS.Timeout;

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
	agent
		.request({
			method: "GET",
			path: `/api/v${APIVersion}${Routes.channelMessages(env["CHANNEL_ID"]!)}`,
			headers: { Authorization: `Bot ${env["DISCORD_TOKEN"]!}` },
			query: { limit: 1 },
		})
		.then((res) => res.body.json())
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
player = new AudioPlayer(connection);
log("Joining voice channel...");
[lastMessageId] = await Promise.all([
	lastMessageId ??
		agent
			.request({
				method: "POST",
				path: `/api/v${APIVersion}${Routes.channelMessages(
					env["CHANNEL_ID"]!,
				)}`,
				body: JSON.stringify({
					flags: MessageFlags.IsComponentsV2,
					components: [{ type: ComponentType.TextDisplay, content: "_ _" }],
				} satisfies RESTPostAPIChannelMessageJSONBody),
				headers: {
					"Authorization": `Bot ${env["DISCORD_TOKEN"]!}`,
					"Content-Type": "application/json",
				},
			})
			.then((res) => res.body.json())
			.then((res) => (res as RESTPostAPIChannelMessageResult).id),
	entersState(
		connection,
		VoiceConnectionStatus.Ready,
		AbortSignal.timeout(20_000),
	),
]);
player.play("https://icstream.rds.radio/rds");
// timeout = setInterval<[{ id: string | undefined }]>(
// 	async (args) => {
// 		try {
// 			let res: Response | Dispatcher.ResponseData = await fetch(
// 				"https://icstream.rds.radio/rds.xspf",
// 			);
// 			if (!res.ok)
// 				throw new Error(`Fetch failed with ${res.status} ${res.statusText}`);
// 			const xml = await res.text();
// 			const [, title, artist, year, newId] =
// 				xml.match(/<title>([^<]+)<\/title>/)?.[1]?.split("*") ?? [];
// 			if (newId === args.id) return;
// 			args.id = newId;
// 			const state = `${decode(title)}${artist ? ` - ${decode(artist)}` : ""}${
// 				year ? ` (${year})` : ""
// 			}`;

// 			[res] = await Promise.all([
// 				fetch(`https://cdnapi.rds.it/v2/site/get_song?idsong=${id}`),
// 				manager.send(0, {
// 					op: GatewayOpcodes.PresenceUpdate,
// 					d: {
// 						activities: [
// 							{
// 								name: "RDS",
// 								type: ActivityType.Listening,
// 								url: "https://rds.it",
// 								state,
// 							},
// 						],
// 						afk: false,
// 						since: null,
// 						status: PresenceUpdateStatus.Online,
// 					},
// 				}),
// 			]);
// 			if (!res.ok)
// 				throw new Error(`Fetch failed with ${res.status} ${res.statusText}`);
// 			const data = (await res.json()) as {
// 				data: { lyrics?: string; id: number; music_log_cover_full: string };
// 			};
// 			res = await client.request({
// 				path: `/api/v${APIVersion}${Routes.channelMessage(
// 					env["CHANNEL_ID"]!,
// 					lastMessageId,
// 				)}`,
// 				method: "PATCH",
// 				headers: {
// 					"Authorization": `Bot ${env["DISCORD_TOKEN"]!}`,
// 					"Content-Type": "application/json",
// 				},
// 				body: JSON.stringify({
// 					flags: MessageFlags.IsComponentsV2,
// 					allowed_mentions: { parse: [] },
// 					components: [
// 						{
// 							type: ComponentType.Section,
// 							components: [
// 								{
// 									type: ComponentType.TextDisplay,
// 									content: `# ${state}\n${
// 										data.data.lyrics !== "none" ? (data.data.lyrics ?? "") : ""
// 									}`,
// 								},
// 							],
// 							accessory: {
// 								type: ComponentType.Thumbnail,
// 								media: {
// 									url:
// 										data.data.id ?
// 											data.data.music_log_cover_full
// 										:	"https://web.rds.it/m/i",
// 								},
// 							},
// 						},
// 					],
// 				} satisfies RESTPatchAPIChannelMessageJSONBody),
// 			});
// 			if (res.statusCode !== 200)
// 				throw new Error(`Fetch failed with ${res.statusCode}`, {
// 					cause: await res.body.text(),
// 				});
// 		} catch (err) {
// 			error(err);
// 		}
// 	},
// 	5_000,
// 	{ id: undefined },
// ).unref();
process.once("SIGINT", () => {
	log("Exiting...");
	// clearInterval(timeout);
	player.destroy();
	log("Player destroyed");
	connection.disconnect();
	connection.destroy();
	manager.destroy();
});
timeEnd("Ready");
