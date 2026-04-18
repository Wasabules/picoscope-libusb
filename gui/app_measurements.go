package main

func (a *App) ComputeMeasurements(data []float64) Measurements {
	if len(data) == 0 {
		return Measurements{}
	}
	minV := data[0]
	maxV := data[0]
	sum := 0.0
	for _, v := range data {
		if v < minV {
			minV = v
		}
		if v > maxV {
			maxV = v
		}
		sum += v
	}
	return Measurements{
		MinMv:  minV,
		MaxMv:  maxV,
		MeanMv: sum / float64(len(data)),
		VppMv:  maxV - minV,
	}
}
