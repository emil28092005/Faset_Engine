local motion = {}

function motion.bob(time, frequency, amplitude)
    return math.sin(time * frequency * 2 * math.pi) * amplitude
end

return motion
