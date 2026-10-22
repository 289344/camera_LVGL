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

let retryTimer = null;
let latestStreaming = false;

pageAddress.textContent = location.origin;

function startStream() {
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
  if (data.streaming) {
    setConnection("online", `直播中 · ${data.fps.toFixed(1)} FPS`);
  } else if (data.board_connected) {
    setConnection("waiting", "板端已连接，等待画面");
  } else {
    setConnection("offline", "等待板端推流");
  }

  placeholder.classList.toggle("is-hidden", latestStreaming);
  liveBadge.classList.toggle("is-live", latestStreaming);

  fpsText.textContent = Number(data.fps || 0).toFixed(1);
  bitrateText.textContent = Number(data.bitrate_kbps || 0).toFixed(1);
  framesText.textContent = Number(data.frames_received || 0).toLocaleString("zh-CN");
  viewersText.textContent = Number(data.viewers || 0).toLocaleString("zh-CN");
  resolutionText.textContent = data.width && data.height
    ? `${data.width} × ${data.height}`
    : "-- × --";
  sourceText.textContent = data.source ? `源地址 ${data.source}` : "源地址 --";
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
