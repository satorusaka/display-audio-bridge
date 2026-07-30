--!nonstrict

local function publish(line)
  local volume, maximum, muted, available, generation, backend =
    line:match("^STATE%s+(%d+)%s+(%d+)%s+(%d+)%s+(%d+)%s+(%d+)%s+(%a+)")
  if volume == nil then return end
  noctalia.state.set("hardware", {
    volume = tonumber(volume),
    maximum = tonumber(maximum),
    muted = muted == "1",
    available = available == "1",
    generation = tonumber(generation),
    backend = backend,
  })
end

local client = noctalia.expandPath("~/.local/bin/ddc-volume-control")
if not noctalia.fileExists(client) then
  noctalia.notifyError("DDC Volume", "Run the repository installer first")
  noctalia.state.set("hardware", {
    volume = 0, maximum = 100, muted = false, available = false,
    generation = 0, backend = "pipewire",
  })
elseif not noctalia.runStream(`{client} watch`, publish) then
  noctalia.notifyError("DDC Volume", "Unable to watch the volume service")
end
