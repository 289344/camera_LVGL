"use strict";

const stream = document.getElementById("stream");
const placeholder = document.getElementById("placeholder");
const connectionPill = document.getElementById("connectionPill");
const connectionText = document.getElementById("connectionText");
const footerStatus = document.getElementById("footerStatus");
const liveBadge = document.querySelector(".live-badge");
const fpsText = document.getElementById("fpsText");
const bitrateText = document.getElementById("bitrateText");
const framesText = document.getElementById("framesText");
const viewersText = document.getElementById("viewersText");
const resolutionText = document.getElementById("resolutionText");
const sourceText = document.getElementById("sourceText");
const updatedText = document.getElementById("updatedText");
const pageAddress = document.getElementById("pageAddress");
const audioButton = document.getElementById("audioButton");
const audioBitrateText = document.getElementById("audioBitrateText");
const audioRateText = document.getElementById("audioRateText");

let retryTimer = null;
let latestStreaming = false;
let latestAudioStreaming = false;
let noVideoViewerSince = null;

let audioDesired = false;
let audioContext = null;
let audioAbortController = null;
let audioLoopPromise = null;
let audioNextStart = 0;
let audioSampleRate = 16000;
let audioChannels = 1;

pageAddress.textContent = location.origin;

function startStream() {
  noVideoViewerSince = null;
  if (retryTimer) {
    clearTimeout(retryTimer);
    retryTimer = null;
  }
  stream.src = `/stream.mjpg?t=${Date.now()}`;
}

function retryStream() {
  if (retryTimer) return;
  retryTimer = setTimeout(() => {
    retryTimer = null;
    startStream();
  }, 1500);
}

function setConnection(mode, text) {
  connectionPill.classList.remove("is-online", "is-waiting", "is-offline");
  connectionPill.classList.add(`is-${mode}`);
  connectionText.textContent = text;
  footerStatus.textContent = text;
}

function renderStatus(data) {
  latestStreaming = Boolean(data.streaming);
  latestAudioStreaming = Boolean(data.audio_streaming);

  if (data.streaming) {
    const audioLabel = latestAudioStreaming
      ? (audioDesired ? "音视频直播中" : "直播中 · 音频可用")
      : "直播中 · 无声音";
    setConnection("online", `${audioLabel} · ${Number(data.fps || 0).toFixed(1)} FPS`);
  } else if (data.board_connected) {
    setConnection("waiting", "板端已连接，等待画面");
  } else {
    setConnection("offline", "等待板端推流");
  }

  placeholder.classList.toggle("is-hidden", latestStreaming);
  liveBadge.classList.toggle("is-live", latestStreaming);

  fpsText.textContent = Number(data.fps || 0).toFixed(1);
  bitrateText.textContent = Number(data.bitrate_kbps || 0).toFixed(1);
  audioBitrateText.textContent = Number(data.audio_bitrate_kbps || 0).toFixed(1);
  framesText.textContent = Number(data.frames_received || 0).toLocaleString("zh-CN");
  viewersText.textContent = Number(data.viewers || 0).toLocaleString("zh-CN");
  if (data.streaming && Number(data.viewers || 0) === 0) {
    if (noVideoViewerSince === null) noVideoViewerSince = Date.now();
    if (Date.now() - noVideoViewerSince > 3000) startStream();
  } else {
    noVideoViewerSince = null;
  }
  resolutionText.textContent = data.width && data.height
    ? `${data.width} × ${data.height}`
    : "-- × --";
  sourceText.textContent = data.source ? `源地址 ${data.source}` : "源地址 --";

  if (data.audio_sample_rate) {
    audioRateText.textContent =
      `${(data.audio_sample_rate / 1000).toFixed(1)}k/${data.audio_channels === 1 ? "单" : "双"}`;
  } else {
    audioRateText.textContent = "--";
  }
  updatedText.textContent = `更新于 ${new Date().toLocaleTimeString("zh-CN", { hour12: false })}`;
}

async function pollStatus() {
  try {
    const response = await fetch("/api/status", { cache: "no-store" });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    renderStatus(await response.json());
  } catch (error) {
    setConnection("offline", "预览服务不可用");
    placeholder.classList.remove("is-hidden");
    liveBadge.classList.remove("is-live");
    updatedText.textContent = "连接失败";
  }
}

function updateAudioButton(waiting = false) {
  audioButton.classList.toggle("is-active", audioDesired);
  if (!audioDesired) {
    audioButton.textContent = "开启声音";
    audioButton.title = "浏览器需要一次点击才能播放直播声音";
  } else if (waiting) {
    audioButton.textContent = "正在接收声音…";
    audioButton.title = "点击关闭声音";
  } else {
    audioButton.textContent = "关闭声音";
    audioButton.title = "声音已开启";
  }
}

function delay(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

function schedulePcm(bytes, sampleRate, channels) {
  if (!audioContext || !audioDesired || bytes.byteLength === 0) return;

  const bytesPerFrame = 2 * channels;
  const frameCount = Math.floor(bytes.byteLength / bytesPerFrame);
  if (frameCount <= 0) return;

  const buffer = audioContext.createBuffer(channels, frameCount, sampleRate);
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  for (let channel = 0; channel < channels; channel += 1) {
    const output = buffer.getChannelData(channel);
    let offset = channel * 2;
    for (let frame = 0; frame < frameCount; frame += 1) {
      output[frame] = view.getInt16(offset, true) / 32768;
      offset += bytesPerFrame;
    }
  }

  const source = audioContext.createBufferSource();
  source.buffer = buffer;
  source.connect(audioContext.destination);

  const now = audioContext.currentTime;
  if (audioNextStart < now + 0.025) audioNextStart = now + 0.12;
  // 页面被挂起后不要补播积压声音，直接丢块，保证直播延迟不会越滚越大。
  if (audioNextStart > now + 0.8) return;

  source.start(audioNextStart);
  audioNextStart += buffer.duration;
}

async function consumePcm(response) {
  if (!response.body) throw new Error("浏览器不支持流式音频");
  const reader = response.body.getReader();
  let carry = new Uint8Array(0);

  while (audioDesired) {
    const { value, done } = await reader.read();
    if (done) break;
    if (!value || value.byteLength === 0) continue;

    let bytes = value;
    if (carry.byteLength) {
      bytes = new Uint8Array(carry.byteLength + value.byteLength);
      bytes.set(carry, 0);
      bytes.set(value, carry.byteLength);
    }

    const frameBytes = 2 * audioChannels;
    const alignedLength = bytes.byteLength - (bytes.byteLength % frameBytes);
    if (alignedLength > 0) {
      schedulePcm(bytes.subarray(0, alignedLength), audioSampleRate, audioChannels);
    }
    carry = bytes.slice(alignedLength);
  }

  if (audioDesired) reader.cancel().catch(() => {});
}

async function audioStreamLoop() {
  try {
    if (!audioContext) {
      const AudioContextClass = window.AudioContext || window.webkitAudioContext;
      if (!AudioContextClass) throw new Error("当前浏览器不支持 Web Audio");
      audioContext = new AudioContextClass();
    }
    if (audioContext.state === "suspended") await audioContext.resume();
    audioNextStart = audioContext.currentTime + 0.12;

    while (audioDesired) {
      const controller = new AbortController();
      audioAbortController = controller;
      updateAudioButton(true);
      try {
        const response = await fetch(`/stream.pcm?t=${Date.now()}`, {
          cache: "no-store",
          signal: controller.signal,
        });
        if (!response.ok) throw new Error(`HTTP ${response.status}`);

        const headerRate = Number(response.headers.get("X-Audio-Sample-Rate"));
        const headerChannels = Number(response.headers.get("X-Audio-Channels"));
        if (Number.isFinite(headerRate) && headerRate >= 8000 && headerRate <= 48000) {
          audioSampleRate = headerRate;
        }
        if (headerChannels === 1 || headerChannels === 2) audioChannels = headerChannels;
        audioNextStart = audioContext.currentTime + 0.12;
        updateAudioButton(false);
        await consumePcm(response);
      } catch (error) {
        if (!audioDesired) break;
        if (error.name !== "AbortError") console.warn("audio stream interrupted", error);
      } finally {
        if (audioAbortController === controller) audioAbortController = null;
      }

      if (audioDesired) await delay(800);
    }
  } catch (error) {
    console.warn("audio unavailable", error);
    audioDesired = false;
  } finally {
    audioAbortController = null;
    updateAudioButton(false);
    audioLoopPromise = null;
  }
}

audioButton.addEventListener("click", () => {
  if (audioDesired) {
    audioDesired = false;
    updateAudioButton(false);
    if (audioAbortController) audioAbortController.abort();
    return;
  }

  audioDesired = true;
  updateAudioButton(true);
  if (!audioLoopPromise) audioLoopPromise = audioStreamLoop();
});

stream.addEventListener("error", retryStream);

document.getElementById("reconnectButton").addEventListener("click", startStream);

document.getElementById("fullscreenButton").addEventListener("click", async () => {
  const shell = document.getElementById("videoShell");
  try {
    if (document.fullscreenElement) {
      await document.exitFullscreen();
    } else {
      await shell.requestFullscreen();
    }
  } catch (error) {
    console.warn("fullscreen unavailable", error);
  }
});

document.getElementById("snapshotButton").addEventListener("click", () => {
  if (!latestStreaming || !stream.naturalWidth || !stream.naturalHeight) return;
  const canvas = document.createElement("canvas");
  canvas.width = stream.naturalWidth;
  canvas.height = stream.naturalHeight;
  canvas.getContext("2d").drawImage(stream, 0, 0);
  const link = document.createElement("a");
  const stamp = new Date().toISOString().replace(/[-:]/g, "").replace(/\..+/, "");
  link.download = `camera-${stamp}.jpg`;
  link.href = canvas.toDataURL("image/jpeg", 0.92);
  link.click();
});

startStream();
pollStatus();
setInterval(pollStatus, 1000);
