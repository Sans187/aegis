// 统一按"服务器时区"处理时间，避免各人浏览器本地时区不同导致错位。
// 偏移(分钟)由服务器 /api/health 提供；未拉取到时用默认 UTC+8(480)。
let tzOffsetMin = 480

export function setTzOffset(min) {
  if (Number.isFinite(min)) tzOffsetMin = min
}
export function getTzOffset() { return tzOffsetMin }

const pad = (n) => String(n).padStart(2, '0')

// epoch 秒 -> 服务器时区的 "YYYY-MM-DD HH:mm:ss"
export function fmt(ts) {
  if (!ts) return '-'
  const d = new Date((ts + tzOffsetMin * 60) * 1000)
  return `${d.getUTCFullYear()}-${pad(d.getUTCMonth() + 1)}-${pad(d.getUTCDate())} ` +
         `${pad(d.getUTCHours())}:${pad(d.getUTCMinutes())}:${pad(d.getUTCSeconds())}`
}

// 日期选择器选的"墙上时间"(用户按服务器时区理解) -> epoch 秒
export function toEpoch(date) {
  if (!date) return null
  const d = new Date(date)
  return Math.floor(Date.UTC(d.getFullYear(), d.getMonth(), d.getDate(),
                             d.getHours(), d.getMinutes(), d.getSeconds()) / 1000) - tzOffsetMin * 60
}
