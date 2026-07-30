--!nonstrict

local state = {
  volume = 0, maximum = 100, muted = false, available = false,
  backend = "pipewire",
}

local function percentage()
  if state.maximum <= 0 then return 0 end
  return math.floor((state.volume * 100 / state.maximum) + 0.5)
end

local function render()
  local target = if state.backend == "ddc" then "Monitor volume" else "Output volume"
  local title = if state.available then `{target}  ·  {percentage()}%`
    else `{target} unavailable`
  local muteText = if state.muted then "Unmute" else "Mute"
  local muteGlyph = if state.muted then "volume" else "volume-3"

  panel.render(ui.column({ gap = 14, padding = 18 }, {
    ui.row({ align = "center", justify = "space_between" }, {
      ui.label({
        text = title, fontSize = 16, fontWeight = "bold",
        color = if state.available then "on_surface" else "outline",
        flexGrow = 1,
      }),
      ui.button({
        text = muteText, glyph = muteGlyph, controlSize = "sm",
        enabled = state.available, selected = state.muted,
        onClick = "toggleMute",
      }),
    }),
    ui.slider({
      min = 0, max = state.maximum, step = 1, value = state.volume,
      controlSize = "lg", enabled = state.available, onChange = "setVolume",
    }),
  }))
end

noctalia.state.watch("hardware", function(value)
  if value ~= nil then state = value; render() end
end)

function onOpen()
  local current = noctalia.state.get("hardware")
  if current ~= nil then state = current end
  render()
end

function setVolume(value)
  local requested = tonumber(value)
  if requested == nil then return end
  requested = math.clamp(math.floor(requested + 0.5), 0, state.maximum)
  state.volume = requested
  render()
  noctalia.runAsync(`~/.local/bin/ddc-volume-control set {requested}`)
end

function toggleMute()
  if state.available then
    noctalia.runAsync("~/.local/bin/ddc-volume-control mute --osd")
  end
end
