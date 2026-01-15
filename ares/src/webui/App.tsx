import {
	createComputed,
	createEffect,
	createRoot,
	createSignal,
	onMount,
	Show,
	Signal,
	type Component,
} from "solid-js";
import { basicSetup, EditorView } from "codemirror";
import { Decoration, keymap, WidgetType } from "@codemirror/view";
import { Compartment, EditorState, Transaction } from "@codemirror/state";

import { lineHighlightEffect, lineHighlightState } from "./LineHighlight";
import { breakpointGutter } from "./Breakpoint";
import { createAsmLinter } from "./AssemblerErrors";
import { defaultKeymap, indentWithTab } from "@codemirror/commands"

import { parser } from "./riscv.grammar";
import { highlighting } from "./GrammarHighlight";
import { LRLanguage, LanguageSupport, indentService, indentUnit } from "@codemirror/language"
import { RegisterTable } from "./RegisterTable";
import { MemoryView } from "./MemoryView";
import { PaneResize } from "./PaneResize";
import { githubLight, githubDark, Theme, Colors, githubHighlightStyle } from './GithubTheme'
import { AsmErrState, buildAsm, continueStep, DebugState, ErrorState, fetchTestcases, getCurrentLine, IdleState, initialRegs, nextStep, quitDebug, RunningState, runNormal, runTestSuite, setWasmRuntime, singleStep, startAutoRun, startStep, startStepTestSuite, StoppedState, testData, TestSuiteState, TestSuiteTableEntry, TEXT_BASE, wasmInterface, wasmRuntime, wasmTestsuite, wasmTestsuiteIdx } from "./EmulatorState";
import { highlightTree } from "@lezer/highlight";

let parserWithMetadata = parser.configure({
	props: [highlighting]
})

export const riscvLanguage = LRLanguage.define({
	parser: parserWithMetadata,
})

let currentTheme: Theme = getDefaultTheme();

export let view: EditorView;
let cmTheme: Compartment = new Compartment();
const lintCompartment = new Compartment();

function updateCss(colors: Colors): void {
	document.getElementById('themestyle').innerHTML = `
.theme-bg {
	background-color: ${colors.base0};
}
.cm-debugging {
	background-color: ${colors.bgorange};
}
.cm-tooltip-lint {
	color: ${colors.base5};
	background-color: ${colors.base0};
	font-family: monospace;
}
.cm-breakpoint-marker {
	background-color: ${colors.red};
}
.theme-bg-hover:hover {
	background-color: ${colors.base1};
}
.theme-bg-active:active {
	background-color: ${colors.base1};
}
.theme-gutter {
	background-color: ${colors.base0};
}
.theme-separator {
	background-color: ${colors.base1};
}
.theme-fg {
	color: ${colors.base4};
}
.theme-fg2 {
	color: ${colors.base3};
}
.theme-scrollbar-slim {
	scrollbar-width: thin;
	scrollbar-color: ${colors.base3} ${colors.base0};
}
.theme-scrollbar {
	scrollbar-color: ${colors.base3} ${colors.base0};
}
.theme-border {
	border-color: ${colors.base2};
}
.frame-highlight {
	background-color: ${colors.bggreen};
}
.theme-testsuccess {
	background-color: ${colors.testgreen};
}
.theme-testfail {
	background-color: ${colors.testred};
}

.cm-header-widget {
}

.theme-style0 { color: ${colors.purp}; }
.theme-style1 { color: ${colors.red}; }
.theme-style2 { color: ${colors.blue}; }
.theme-style3 { color: ${colors.orange}; }
.theme-style4 { color: ${colors.base4}; }
.theme-style5 { color: ${colors.orange}; }
.theme-style6 { color: ${colors.lightblue}; }
.theme-style7 { color: ${colors.base3}; }
.theme-style8 { font-weight: bold; }
.theme-style9 { font-style: italic; }
.theme-style10 { text-decoration: line-through; }
.theme-style11 { text-decoration: underline; }
.theme-style12 { color: ${colors.base3}; text-decoration: underline; }
.theme-style13 { color: ${colors.orange}; }
.theme-style14 { color: ${colors.green}; }
.theme-style15 { color: ${colors.base5}; }

.cm-header-widget {
	padding-bottom: 1rem;
}

.cm-header-widget > a {
	background-color: ${colors.base1};
	font-style: italic;
	font-weight: bold;
}

.cm-header-widget > div {
	background-color: ${colors.base1};
	display: inline-block;
	padding-left: 0.5em;
	padding-right: 0.5em;
	padding-top: 0.25em;
	padding-bottom: 0.25em;
}


@keyframes fadeHighlight {
	from {
		background-color: ${colors.bgorange};
	}
	to {
	}
}
.animate-fade-highlight {
	animation: fadeHighlight 1s forwards;
}
`;
}

declare global {
  interface Window {
    urlParams: URLSearchParams;
	testsuiteName: string;
  }
}
export const testsuiteName = window.testsuiteName;
const localStorageKey = window.testsuiteName ? ("savedtext-" + window.testsuiteName) : "savedtext";

window.addEventListener("DOMContentLoaded", () => {
	updateCss(currentTheme.colors);
});

function changeTheme(theme: Theme): void {
	// TODO: the theme reconfigure would be unnecessary if i just have all the styles in one place
	view.dispatch({ effects: cmTheme.reconfigure(theme.cmTheme) });
	updateCss(theme.colors);
}

function getDefaultTheme() {
	const savedTheme = localStorage.getItem("theme");
	if (savedTheme && savedTheme == "GithubDark") return githubDark;
	else if (savedTheme && savedTheme == "GithubLight") return githubLight;

	const prefersDark = window.matchMedia("(prefers-color-scheme: dark)").matches;
	if (prefersDark) return githubDark;
	else return githubLight;
}

function doChangeTheme(): void {
	if (currentTheme == githubDark) {
		currentTheme = githubLight;
		changeTheme(githubLight);
		localStorage.setItem("theme", "GithubLight");
	}
	else if (currentTheme == githubLight) {
		currentTheme = githubDark;
		changeTheme(githubDark);
		localStorage.setItem("theme", "GithubDark");
	}
}

const isMac = navigator.platform.toLowerCase().includes('mac');
const prefixStr = isMac ? "Ctrl-Shift" : "Ctrl-Alt"

const VGA_WIDTH = 160;
const VGA_HEIGHT = 120;
const VGA_SCALE = 3;
const VGA_TITLE = "ARES VGA";
const GIF_STRIP_SYSCALL = 100;

let gifInputRef: HTMLInputElement;
const [vgaVisible, setVgaVisible] = createSignal(false);
let vgaCanvas: HTMLCanvasElement | null = null;
let vgaCtx: CanvasRenderingContext2D | null = null;
let vgaImageData: ImageData | null = null;
let vgaStatus: HTMLDivElement | null = null;
let vgaTimer: number | null = null;
let vgaBuffer: Uint8Array | null = null;
let gifAutoCancel: (() => void) | null = null;
let vgaWindow: HTMLDivElement | null = null;
let vgaPositionInitialized = false;
const [vgaPosition, setVgaPosition] = createSignal({ x: 0, y: 0 });
let vgaDragOffset: { x: number; y: number } | null = null;

function stopVgaPlayback(): void {
	if (vgaTimer !== null) {
		clearTimeout(vgaTimer);
		vgaTimer = null;
	}
	if (gifAutoCancel) {
		gifAutoCancel();
		gifAutoCancel = null;
	}
}

function initVgaCanvas(): boolean {
	if (!vgaCanvas) {
		alert("Unable to create VGA canvas.");
		return false;
	}
	vgaCanvas.width = VGA_WIDTH;
	vgaCanvas.height = VGA_HEIGHT;
	vgaCanvas.style.width = `${VGA_WIDTH * VGA_SCALE}px`;
	vgaCanvas.style.height = `${VGA_HEIGHT * VGA_SCALE}px`;
	vgaCanvas.style.imageRendering = "pixelated";
	vgaCtx = vgaCanvas.getContext("2d");
	if (!vgaCtx) {
		alert("Unable to initialize VGA context.");
		return false;
	}
	vgaImageData = vgaCtx.createImageData(VGA_WIDTH, VGA_HEIGHT);
	return true;
}

async function ensureVgaReady(): Promise<boolean> {
	if (vgaCanvas && vgaCtx && vgaImageData) {
		return true;
	}
	setVgaVisible(true);
	await new Promise<void>((resolve) => requestAnimationFrame(() => resolve()));
	if (!vgaCanvas) {
		await new Promise<void>((resolve) => setTimeout(resolve, 0));
	}
	if (!vgaPositionInitialized && vgaWindow) {
		const rect = vgaWindow.getBoundingClientRect();
		const x = Math.max(0, Math.round((window.innerWidth - rect.width) / 2));
		const y = Math.max(0, Math.round((window.innerHeight - rect.height) / 2));
		setVgaPosition({ x, y });
		vgaPositionInitialized = true;
	}
	if (vgaCanvas && (!vgaCtx || !vgaImageData)) {
		return initVgaCanvas();
	}
	if (!vgaCanvas) {
		alert("Unable to create VGA canvas.");
		return false;
	}
	return true;
}

function closeVgaWindow(): void {
	stopVgaPlayback();
	setVgaVisible(false);
	vgaCtx = null;
	vgaImageData = null;
	vgaPositionInitialized = false;
	if (vgaStatus) {
		vgaStatus.textContent = "Waiting for GIF...";
	}
}

function renderVga(): void {
	if (!vgaBuffer || !vgaCtx || !vgaImageData) return;
	vgaImageData.data.set(vgaBuffer.subarray(0, vgaImageData.data.length));
	vgaCtx.putImageData(vgaImageData, 0, 0);
}

function startVgaDrag(event: PointerEvent): void {
	if (!vgaWindow) return;
	event.preventDefault();
	event.stopPropagation();
	const rect = vgaWindow.getBoundingClientRect();
	vgaDragOffset = { x: event.clientX - rect.left, y: event.clientY - rect.top };
	const handleMove = (moveEvent: PointerEvent) => {
		if (!vgaDragOffset || !vgaWindow) return;
		const bounds = vgaWindow.getBoundingClientRect();
		const nextX = moveEvent.clientX - vgaDragOffset.x;
		const nextY = moveEvent.clientY - vgaDragOffset.y;
		const maxX = Math.max(0, window.innerWidth - bounds.width);
		const maxY = Math.max(0, window.innerHeight - bounds.height);
		setVgaPosition({
			x: Math.min(Math.max(0, Math.round(nextX)), Math.round(maxX)),
			y: Math.min(Math.max(0, Math.round(nextY)), Math.round(maxY))
		});
	};
	const handleUp = () => {
		vgaDragOffset = null;
		window.removeEventListener("pointermove", handleMove);
		window.removeEventListener("pointerup", handleUp);
	};
	window.addEventListener("pointermove", handleMove);
	window.addEventListener("pointerup", handleUp);
}

function buildGifAssembly(): string {
	const vgaSize = VGA_WIDTH * VGA_HEIGHT * 4;
	return `# Auto-generated GIF runner
.text
.globl _start
_start:
	li ra, 0
	li sp, 0x7ffff000
	li gp, 0
	li tp, 0
	li t0, 0
	li t1, 0
	li t2, 0
	li s0, 0
	li s1, 0
	li a0, 0
	li a1, 0
	li a2, 0
	li a3, 0
	li a4, 0
	li a5, 0
	li a6, 0
	li a7, 0
	li s2, 0
	li s3, 0
	li s4, 0
	li s5, 0
	li s6, 0
	li s7, 0
	li s8, 0
	li s9, 0
	li s10, 0
	li s11, 0
	li t3, 0
	li t4, 0
	li t5, 0
	li t6, 0
	li a7, ${GIF_STRIP_SYSCALL}
	ecall
	beq a0, zero, done
	mv s0, a0
	lw s1, 0(s0)
	lw s2, 4(s0)
	addi s3, s0, 12
	la s4, _GIF_BASE
	la s5, _VGA_BASE
	li s6, 0
	li s7, ${vgaSize}
frame_loop:
	beq s1, zero, done
	lw t0, 0(s3)
	add t1, s4, t0
	mv t2, s5
	mv t3, s2
	bltu t3, s7, len_ok
	mv t3, s7
len_ok:
	andi t4, t3, -4
	li t5, 0
word_loop:
	beq t5, t4, tail
	lw t6, 0(t1)
	sw t6, 0(t2)
	addi t1, t1, 4
	addi t2, t2, 4
	addi t5, t5, 4
	j word_loop
tail:
	beq t4, t3, frame_done
	sub t0, t3, t4
	li t5, 0
tail_loop:
	beq t5, t0, frame_done
	lbu t6, 0(t1)
	sb t6, 0(t2)
	addi t1, t1, 1
	addi t2, t2, 1
	addi t5, t5, 1
	j tail_loop
frame_done:
	addi s6, s6, 1
	addi s3, s3, 4
	bne s6, s1, frame_loop
	li s6, 0
	addi s3, s0, 12
	j frame_loop
done:
	li a7, 93
	li a0, 0
	ecall
`;
}

function openGifPicker(): void {
	gifInputRef?.click();
}

async function loadGifToVga(file: File): Promise<void> {
	if (!view) {
		alert("Editor is not ready yet.");
		return;
	}
	if (!(await ensureVgaReady())) return;
	stopVgaPlayback();

	if (vgaStatus) {
		vgaStatus.textContent = `Loading ${file.name}...`;
	}

	const buffer = await file.arrayBuffer();
	const gifBytes = new Uint8Array(buffer);

	const asm = buildGifAssembly();
	view.dispatch({
		changes: { from: 0, to: view.state.doc.length, insert: asm }
	});

	await buildAsm(wasmRuntime, setWasmRuntime);
	if (wasmRuntime.status == "asmerr") return;

	const gifPtr = wasmInterface.gifPtr?.[0] ?? 0;
	const gifLen = wasmInterface.gifLen?.[0] ?? 0;
	const vgaPtr = wasmInterface.vgaPtr?.[0] ?? 0;
	const vgaLen = wasmInterface.vgaLen?.[0] ?? 0;
	if (!gifPtr || !gifLen || !vgaPtr || !vgaLen) {
		alert("VGA/GIF memory not initialized yet.");
		return;
	}
	if (vgaLen < VGA_WIDTH * VGA_HEIGHT * 4) {
		alert("VGA buffer is smaller than expected.");
		return;
	}
	if (gifBytes.length > gifLen) {
		alert(`GIF is too large for memory (${gifBytes.length} > ${gifLen} bytes).`);
		return;
	}

	if (!("ImageDecoder" in window)) {
		alert("This browser does not support ImageDecoder for GIF playback.");
		return;
	}

	const decoder = new ImageDecoder({ data: buffer, type: "image/gif" });
	await decoder.tracks.ready;
	const track = decoder.tracks.selectedTrack;
	const frameCount = Math.max(1, track?.frameCount ?? 1);
	const frameSize = VGA_WIDTH * VGA_HEIGHT * 4;
	const tableOffset = (gifBytes.length + 3) & ~3;
	const tableSize = 12 + frameCount * 4;
	const frameDataOffset = (tableOffset + tableSize + 3) & ~3;
	const totalSize = frameDataOffset + frameCount * frameSize;
	if (totalSize > gifLen) {
		decoder.close();
		alert("Decoded frames do not fit in GIF memory.");
		return;
	}

	const offscreen = document.createElement("canvas");
	offscreen.width = VGA_WIDTH;
	offscreen.height = VGA_HEIGHT;
	const offCtx = offscreen.getContext("2d");
	if (!offCtx) {
		decoder.close();
		alert("Unable to create GIF decoder canvas.");
		return;
	}

	const framesData = new Uint8Array(frameCount * frameSize);
	for (let i = 0; i < frameCount; i++) {
		const result = await decoder.decode({ frameIndex: i });
		const frame = result.image;
		const frameWidth = frame.displayWidth || VGA_WIDTH;
		const frameHeight = frame.displayHeight || VGA_HEIGHT;
		const scale = Math.min(VGA_WIDTH / frameWidth, VGA_HEIGHT / frameHeight);
		const drawWidth = Math.round(frameWidth * scale);
		const drawHeight = Math.round(frameHeight * scale);
		const offsetX = Math.round((VGA_WIDTH - drawWidth) / 2);
		const offsetY = Math.round((VGA_HEIGHT - drawHeight) / 2);
		offCtx.clearRect(0, 0, VGA_WIDTH, VGA_HEIGHT);
		offCtx.drawImage(frame, offsetX, offsetY, drawWidth, drawHeight);
		const imageData = offCtx.getImageData(0, 0, VGA_WIDTH, VGA_HEIGHT);
		framesData.set(imageData.data, i * frameSize);
		frame.close();
	}
	decoder.close();

	const gifBuffer = wasmInterface.createU8(gifPtr);
	gifBuffer.set(gifBytes);
	const tableView = new DataView(gifBuffer.buffer, gifPtr + tableOffset, tableSize);
	tableView.setUint32(0, frameCount, true);
	tableView.setUint32(4, frameSize, true);
	tableView.setUint32(8, 0, true);
	for (let i = 0; i < frameCount; i++) {
		tableView.setUint32(12 + i * 4, frameDataOffset + i * frameSize, true);
	}
	gifBuffer.set(framesData, frameDataOffset);
	const gifBase = wasmInterface.gifBase?.[0] ?? 0;
	if (wasmInterface.gifUsed) wasmInterface.gifUsed[0] = gifBytes.length;
	if (wasmInterface.gifBodyPtr) wasmInterface.gifBodyPtr[0] = gifBase + tableOffset;
	if (wasmInterface.gifBodyLen) wasmInterface.gifBodyLen[0] = tableSize;

	vgaBuffer = wasmInterface.createU8(vgaPtr).subarray(0, vgaLen);
	vgaBuffer.fill(0);
	renderVga();

	if (vgaStatus) {
		vgaStatus.textContent = "Running GIF via CPU...";
	}

	gifAutoCancel = startAutoRun(setWasmRuntime, renderVga, 5000, true);
}

function handleGifInput(event: Event): void {
	const input = event.target as HTMLInputElement;
	const file = input.files?.[0];
	if (!file) return;
	input.value = "";
	loadGifToVga(file).catch((err) => {
		console.error(err);
		alert(`Failed to load GIF: ${String(err)}`);
	});
}

window.addEventListener('keydown', (event) => {
	// FIXME: this is deprecated but i'm not sure what is the correct successor
	const prefix = isMac ? (event.ctrlKey && event.shiftKey) : (event.ctrlKey && event.altKey);

	if (wasmRuntime.status == "debug" && prefix && event.key.toUpperCase() == 'S') {
		event.preventDefault();
		singleStep(wasmRuntime, setWasmRuntime);
	}
	else if (wasmRuntime.status == "debug" && prefix && event.key.toUpperCase() == 'N') {
		event.preventDefault();
		nextStep(wasmRuntime, setWasmRuntime);
	}
	else if (wasmRuntime.status == "debug" && prefix && event.key.toUpperCase() == 'C') {
		event.preventDefault();
		continueStep(wasmRuntime, setWasmRuntime);
	}
	else if (wasmRuntime.status == "debug" && prefix && event.key.toUpperCase() == 'X') {
		event.preventDefault();
		quitDebug(wasmRuntime, setWasmRuntime);
	}
	if (testData) {
		if (prefix && event.key.toUpperCase() == 'R') {
			event.preventDefault();
			runTestSuite(wasmRuntime, setWasmRuntime);
		}
	} else {
		if (prefix && event.key.toUpperCase() == 'R') {
			event.preventDefault();
			runNormal(wasmRuntime, setWasmRuntime);
		}
		else if (prefix && event.key.toUpperCase() == 'D') {
			event.preventDefault();
			startStep(wasmRuntime, setWasmRuntime);
		}
	}
});

const Navbar: Component = () => {
	return (
		<nav class="flex-none theme-gutter">
			<div class="mx-auto px-2">
				<div class="flex items-center h-10">
					<div class="flex-shrink-0">
						<h1 class="text-xl font-bold theme-fg">ARES</h1>
					</div>
					<div class="flex-shrink-0 mx-auto"></div>
					<Show when={wasmRuntime.status == "debug" ? wasmRuntime : null}>{debugRuntime => <>
						<button
							on:click={() => singleStep(debugRuntime(), setWasmRuntime)}
							class="cursor-pointer flex-0-shrink flex material-symbols-outlined theme-fg theme-bg-hover theme-bg-active"
							title={`Step into (${prefixStr}-S)`}
						>
							step_into
						</button>
						<button
							on:click={() => nextStep(debugRuntime(), setWasmRuntime)}
							class="cursor-pointer flex-0-shrink flex material-symbols-outlined theme-fg theme-bg-hover theme-bg-active"
							title={`Step over/Next (${prefixStr}-N)`}
						>
							step_over
						</button>
						<button
							on:click={() => continueStep(debugRuntime(), setWasmRuntime)}
							class="cursor-pointer flex-0-shrink flex material-symbols-outlined theme-fg theme-bg-hover theme-bg-active"
							title={`Continue (${prefixStr}-C)`}
						>
							resume
						</button>
						<button
							on:click={() => quitDebug(debugRuntime(), setWasmRuntime)}
							class="cursor-pointer flex-0-shrink flex material-symbols-outlined theme-fg theme-bg-hover theme-bg-active"
							title={`Exit debugging (${prefixStr}-X)`}
						>
							stop
						</button>
						<div class="cursor-pointer flex-shrink-0 mx-auto"></div></>
					}
					</Show>
					<button
						on:click={openGifPicker}
						class="cursor-pointer flex-0-shrink flex material-symbols-outlined theme-fg theme-bg-hover theme-bg-active"
						title="Load GIF to VGA"
					>
						<span class="text-[18px] leading-none">gif_box</span>
					</button>
					<input
						ref={gifInputRef}
						type="file"
						accept="image/gif"
						class="hidden"
						on:change={handleGifInput}
					/>
					<button
						on:click={doChangeTheme}
						class="cursor-pointer flex-0-shrink flex material-symbols-outlined theme-fg theme-bg-hover theme-bg-active"
						title="Change theme"
					>
						dark_mode
					</button>
					<Show when={testsuiteName}>
						<button
							on:click={() => runTestSuite(wasmRuntime, setWasmRuntime)}
							class="cursor-pointer flex-0-shrink flex material-symbols-outlined theme-fg theme-bg-hover theme-bg-active"
							title={`Run tests (${prefixStr}-R)`}
						>
							play_circle
						</button>
					</Show>
					<Show when={!testsuiteName}>
						<button
							on:click={() => runNormal(wasmRuntime, setWasmRuntime)}
							class="cursor-pointer flex-0-shrink flex material-symbols-outlined theme-fg theme-bg-hover theme-bg-active"
							title={`Run (${prefixStr}-R)`}
						>
							play_circle
						</button>
						<button
							on:click={() => startStep(wasmRuntime, setWasmRuntime)}
							class="cursor-pointer flex-0-shrink flex material-symbols-outlined theme-fg theme-bg-hover theme-bg-active"
							title={`Debug (${prefixStr}-D)`}
						>
							arrow_forward
						</button>
					</Show>
				</div>
			</div>
		</nav>
	);
};

const dummyIndent = indentService.of((context, pos) => {
	if (pos < 0 || pos > context.state.doc.length) return null;
	let line = context.lineAt(pos);
	if (line.from === 0) return 0;
	let prevLine = context.lineAt(line.from - 1);
	let match = /^\s*/.exec(prevLine.text);
	if (!match) return 0;
	let cnt = 0;
	for (let i = 0; i < match[0].length; i++) {
		if (match[0][i] == '\t') cnt = cnt + 4 - cnt % 4;
		else cnt += 1;
	}
	return cnt;
});

const tabKeymap = keymap.of([{
	key: "Tab",
	run(view) {
		const { state, dispatch } = view;
		const { from, to } = state.selection.main;
		// insert tab instead of indenting if it's a single line selection
		// messy code for indenting the start of the line with spaces, but keep tabs for the tabulation inside the line
		let lineIsEmpty = true;
		let str = state.doc.toString();
		for (let i = state.doc.lineAt(from).from; i < from; i++) {
			if (str[i] != '\t' && str[i] != ' ' && str[i] != '\n') {
				lineIsEmpty = false;
				break;
			}
		}
		if (!lineIsEmpty && (from == to || state.doc.lineAt(from).number == state.doc.lineAt(to).number)) {
			dispatch(state.update(state.replaceSelection("\t"), {
				scrollIntoView: true,
				userEvent: "input"
			}));
			return true;
		}
		return false;
	}
}]);

function toHex(arg: number): string {
	return "0x" + arg.toString(16).padStart(8, "0");
}

const BacktraceCall: Component<{ name: string, args: number[], sp: number }> = (props) => {
	return <div class="flex">
		<div class="font-bold pr-1">{props.name}</div>
		<div class="flex flex-row flex-wrap">
			<div class="theme-fg2">args=</div>
			<div class="pr-1">{toHex(props.args[0])}</div>
			<div class="pr-1">{toHex(props.args[1])}</div>
			<div class="pr-1">{toHex(props.args[2])}</div>
			<div class="pr-1">{toHex(props.args[3])}</div>
			<div class="pr-1">{toHex(props.args[4])}</div>
			<div class="pr-1">{toHex(props.args[5])}</div>
			<div class="pr-1">{toHex(props.args[6])}</div>
			<div class="pr-1">{toHex(props.args[7])}</div>
			<div class="theme-fg2">sp=</div>
			<div class="pr-1">{toHex(props.sp)}</div>
		</div>
	</div>
};


const BacktraceView = (state: DebugState | ErrorState) => {
	return <div class="w-full h-full font-mono text-sm overflow-auto theme-scrollbar-slim flex flex-col">
		{[...state.shadowStack].reverse().map(ent => <BacktraceCall name={ent.name} args={ent.args} sp={ent.sp} />)}
	</div>;
};

const Editor: Component = () => {
	let editor: HTMLDivElement | undefined;
	// enable and disable linter based on debugMode() and hasError()
	createComputed(() => {
		const disable = wasmRuntime.status == "debug" || wasmRuntime.status == "error";
		if (view == undefined) return;
		if (disable) {
			view.dispatch({
				effects: lintCompartment.reconfigure([])
			});
		} else {
			view.dispatch({
				effects: lintCompartment.reconfigure(createAsmLinter())
			});
		}
	})
	createComputed(() => {
		let lineno = 0;
		if (wasmRuntime.status == "debug" || wasmRuntime.status == "error") {
			lineno = getCurrentLine(wasmRuntime);
		}
		if (!view) return;
		view.dispatch({
			effects: lineHighlightEffect.of(lineno), // disable the line highlight, as line numbering starts from 1 
		});
	})

	onMount(async () => {
		await fetchTestcases();

		const theme = EditorView.theme({
			"&.cm-editor": { height: "100%" },
			".cm-scroller": { overflow: "auto" },
		});
		const savedText = localStorage.getItem(localStorageKey) || "";
		const state = EditorState.create({
			doc: savedText,
			extensions: [
				tabKeymap,
				new LanguageSupport(riscvLanguage, [dummyIndent]),
				lintCompartment.of(createAsmLinter()),
				breakpointGutter, // must be first so it's the first gutter
				basicSetup,
				theme,
				EditorView.editorAttributes.of({ style: "font-size: 1.4em" }),
				cmTheme.of(currentTheme.cmTheme),
				[lineHighlightState],
				indentUnit.of("    "),
				keymap.of([...defaultKeymap, indentWithTab]),
				headerDecoration(),
				EditorView.lineWrapping
			],
		});
		view = new EditorView({ state, parent: editor });

		setInterval(() => {
			localStorage.setItem(localStorageKey, view.state.doc.toString());
		}, 1000);
	});

	return <main
		class="w-full h-full overflow-hidden theme-scrollbar" style={{ contain: "strict" }}
		ref={editor} />;
}

let consoleText = (_runtime: IdleState | RunningState | DebugState | ErrorState | StoppedState | AsmErrState | TestSuiteState) => {
	if (_runtime.status == "idle" || _runtime.status == "testsuite") return "";
	return _runtime.consoleText;
}

const TestSuiteViewer = (table: TestSuiteTableEntry[], currentDebuggingEntry: number) => {
	return (
		<div class="theme-scrollbar theme-bg theme-fg overflow-x-auto overflow-y-auto w-full h-full">
			<table class="table w-full max-w-full h-full min-w-full border-collapse rounded-lg ">
				<thead class=" ">
					<tr class="  text-left theme-fg border-b theme-border">
						<th class="w-[8ch] px-2 py-1 font-semibold theme-fg">status</th>
						<th class="w-[14ch] px-2 py-1 font-semibold theme-fg">input</th>
						<th class="w-[8ch] px-2 py-1 font-semibold theme-fg whitespace-nowrap">expected</th>
						<th class="w-[14ch] px-2 py-1 font-semibold theme-fg whitespace-nowrap">yours</th>
					</tr>
				</thead>
				<tbody class=" ">
					{table.map((testcase, index) => {
						const passed = testcase.output === testcase.userOutput;
						const errorType = testcase.runErr ? "crashed" : "mismatched";
						return (
							<tr
								class={`  border-b theme-border ${passed ? 'theme-testsuccess' : 'theme-testfail'}`}
							>
								<td class="px-2">
									{passed ?
										<div class="flex flex-col">
											<span class="text-sm">success</span>
											<button class="text-left text-sm hover:font-semibold " on:click={() => startStepTestSuite(wasmRuntime, setWasmRuntime, index)}>
												{currentDebuggingEntry === index ? "debugging" : "debug it"}
											</button>
										</div> :
										<div class="flex flex-col">
											<span class="text-sm">{errorType}</span>
											<button class="text-left text-sm underline hover:font-semibold " on:click={() => startStepTestSuite(wasmRuntime, setWasmRuntime, index)}>
												{currentDebuggingEntry === index ? "debugging" : "debug it"}
											</button>
										</div>}
								</td>
								<td class="px-1 py-1.5 text-sm">
									<code class="text-xs font-mono rounded whitespace-pre-wrap break-words max-w-full block">
										{testcase.input}
									</code>
								</td>
								<td class="px-1 py-1.5 text-sm">
									<code class="px-1 py-1.5 rounded text-xs font-mono whitespace-pre-wrap break-words max-w-full block">
										{testcase.output}
									</code>
								</td>
								<td class="px-1 py-1.5 text-sm">
									<code class={`px-1 py-1.5 rounded text-xs font-mono whitespace-pre-wrap break-words max-w-full block `}>
										{testcase.userOutput}
									</code>
								</td>
							</tr>
						);
					})}
				</tbody>
			</table>
		</div>
	);
};


function createHighlightedText(code: string, syntaxHighlight: boolean) {
	let block = document.createElement("div")
	block.className = "cm-header-code"

	let pos = 0
	if (syntaxHighlight) {
		let tree = riscvLanguage.parser.parse(code)
		highlightTree(tree, githubHighlightStyle, (from, to, classes) => {
			if (from > pos) block.appendChild(document.createTextNode(code.slice(pos, from)))
			let span = document.createElement("span")
			span.className = classes
			span.textContent = code.slice(from, to)
			block.appendChild(span)

			pos = to
		})
	}
	if (pos < code.length) block.appendChild(document.createTextNode(code.slice(pos)))
	return block
}

function parseFormat(fmt: string, container): void {
	const parts = fmt.split(/(```[\s\S]*?```)/);

	for (const part of parts) {
		if (part.startsWith('```') && part.endsWith('```')) {
			let codeContent = part.slice(3, -3);
			let highlight = false;
			if (codeContent.startsWith("riscv")) {
				highlight = true;
				codeContent = codeContent.slice(5);
			}
			if (codeContent.startsWith('\r\n')) {
				codeContent = codeContent.slice(2);
			} else if (codeContent.startsWith('\n')) {
				codeContent = codeContent.slice(1);
			}

			const highlightedElement = createHighlightedText(codeContent, highlight);
			container.appendChild(highlightedElement);
		} else {
			parseInlineCode(part, container);
		}
	}
}

function parseInlineCode(text: string, container: HTMLElement): void {
	// Split by single backticks to handle inline code
	const parts = text.split(/(`[^`]+`)/);

	for (const part of parts) {
		if (part.startsWith('`') && part.endsWith('`')) {
			const codeContent = part.slice(1, -1);
			const anchor = document.createElement('a');
			anchor.textContent = codeContent;
			container.appendChild(anchor);
		} else if (part) {
			const textNode = document.createTextNode(part);
			container.appendChild(textNode);
		}
	}
}

class HeaderWidget extends WidgetType {
	constructor() {
		super();
	}

	toDOM(view: EditorView) {
		const container = document.createElement("div");
		container.className = "cm-header-widget";
		let tdata = testData;
		let assignment = tdata ? tdata.assignment : "";
		parseFormat(assignment, container);
		return container;
	}

	ignoreEvent() {
		return true;
	}
}




function headerDecoration() {
	return EditorView.decorations.of(Decoration.set([
		Decoration.widget({
			widget: new HeaderWidget(),
			side: -1,
			block: true
		}).range(0)
	]))
}


const App: Component = () => {
	return (
		<div class="fullsize flex flex-col justify-between overflow-hidden">
			<Navbar />
			<div
				class={`fixed inset-0 z-50 flex items-center justify-center bg-black/60 ${vgaVisible() ? "" : "hidden"}`}
				on:click={closeVgaWindow}
			>
			<div
				ref={vgaWindow}
				class="theme-bg theme-fg border theme-border rounded-lg p-4"
				style={{ position: "absolute", left: `${vgaPosition().x}px`, top: `${vgaPosition().y}px` }}
				on:click={(event) => event.stopPropagation()}
			>
				<div class="flex items-center justify-between mb-2 cursor-move" onPointerDown={startVgaDrag}>

						<div class="font-semibold">ARES VGA</div>
						<button
							on:click={closeVgaWindow}
							class="cursor-pointer theme-fg theme-bg-hover theme-bg-active"
							title="Close VGA"
						>
							close
						</button>
					</div>
					<div ref={vgaStatus} class="text-sm text-center mb-2">
						Waiting for GIF...
					</div>
					<canvas ref={vgaCanvas} style={{ display: "block", margin: "0 auto", background: "#000" }}></canvas>
				</div>
			</div>
			<div class="grow flex overflow-hidden">
				<PaneResize firstSize={0.5} direction="horizontal" second={true}>
					{() =>
						<PaneResize firstSize={0.65} direction="vertical"
							second={wasmTestsuite().length > 0}>
							{() => <PaneResize firstSize={0.85} direction="vertical"
								second={((wasmRuntime && (wasmRuntime.status == "debug" || wasmRuntime.status == "error")) && wasmRuntime.shadowStack.length > 0) ? wasmRuntime : null}>
								{() => <Editor />}
								{wasmRuntime => BacktraceView(wasmRuntime)}
							</PaneResize>}
							{() => TestSuiteViewer(wasmTestsuite(), wasmTestsuiteIdx())}
						</PaneResize>
					}

					{() => <PaneResize firstSize={0.75} direction="vertical" second={true}>
						{() => <PaneResize firstSize={0.55} direction="horizontal" second={true}>
							{() => <RegisterTable pc={(wasmRuntime.status == "idle" || wasmRuntime.status == "asmerr" || wasmRuntime.status == "testsuite") ? TEXT_BASE : wasmRuntime.pc}
								regs={(wasmRuntime.status == "idle" || wasmRuntime.status == "asmerr" || wasmRuntime.status == "testsuite") ? initialRegs : wasmRuntime.regs}
								regWritten={wasmInterface.regWritten ? wasmInterface.regWritten[0] : 0} />}
							{() => <MemoryView version={() => wasmRuntime.version}
								writeAddr={wasmInterface.memWrittenAddr ? wasmInterface.memWrittenAddr[0] : 0}
								writeLen={wasmInterface.memWrittenLen ? wasmInterface.memWrittenLen[0] : 0}
								sp={wasmInterface.regsArr ? wasmInterface.regsArr[2 - 1] : 0}
								load={wasmInterface.emu_load}
							/>}
						</PaneResize>}
						{() => (<div
							innerText={consoleText(wasmRuntime) ? consoleText(wasmRuntime) : "Console output will go here..."}
							class={"w-full h-full font-mono text-md overflow-auto theme-scrollbar theme-bg " + (consoleText(wasmRuntime) ? "theme-fg" : "theme-fg2")}
						></div>)}
					</PaneResize>}
				</PaneResize>
			</div>
		</div>
	);
};

export default App;
