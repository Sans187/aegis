// 按需引入 ECharts，控制打包体积
import * as echarts from 'echarts/core'
import { BarChart, LineChart } from 'echarts/charts'
import {
  GridComponent,
  TooltipComponent,
  LegendComponent,
  DataZoomComponent,
  MarkLineComponent,
} from 'echarts/components'
import { LabelLayout } from 'echarts/features'
import { CanvasRenderer } from 'echarts/renderers'

echarts.use([
  BarChart, LineChart,
  GridComponent, TooltipComponent, LegendComponent, DataZoomComponent, MarkLineComponent,
  LabelLayout, CanvasRenderer,
])

export default echarts
