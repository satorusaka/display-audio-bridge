--!nonstrict

local state = {
  volume = 0, maximum = 100, muted = false, available = false,
  backend = "pipewire",
}

local function percentage()
  if state.maximum <= 0 then return 0 end
  return math.floor((state.volume * 100 / state.maximum) + 0.5)
end

local function glyph()
  if not state.available then return "volume-off" end
  if state.muted or state.volume == 0 then return "volume-3" end
  if percentage() < 67 then return "volume-2" end
  return "volume"
end

local function render()
  local color = if not state.available then "outline"
    elseif state.muted then "error" else "on_surface"
  local label = if state.available then `{percentage()}%` else "--"
  barWidget.render(ui.row({ gap = 5, align = "center" }, {
    ui.glyph({ name = glyph(), size = 16, color = color }),
    ui.label({ text = label, color = color }),
  }))
end

noctalia.state.watch("hardware", function(value)
  if value ~= nil then state = value; render() end
end)

function update()
  local current = noctalia.state.get("hardware")
  if current ~= nil then state = current end
  render()
end

function onClick()
  noctalia.togglePanel("satorusaka/ddc-volume:panel")
end

function onRightClick()
  noctalia.runAsync("~/.local/bin/ddc-volume-control mute --osd")
end

function onScroll(axis, steps)
  if axis ~= "vertical" or steps == 0 or not state.available then return end
  local command = if steps < 0 then "up" else "down"
  for _ = 1, math.abs(math.floor(steps)) do
    noctalia.runAsync(`~/.local/bin/ddc-volume-control {command} --osd`)
  end
end

render()
