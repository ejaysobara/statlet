namespace StatletSettings;

// The island's spring for a given speed and bounce (0-100), the same numbers as SpringFromFeel in island/src/settings.h.
// Shared fixtures in tests/fixtures/spring.json keep the two copies equal.
public static class Motion
{
    // Speed doubles the stiffness every 40 steps; the defaults 40 and 60 give stiffness 420, damping 34.
    public static double Stiffness(double speed) => 420 * Math.Pow(2, (speed - 60) / 40);

    // Bounce lowers the damping ratio from 1 (no overshoot) to 0.57.
    public static double Damping(double speed, double bounce) => (1 - bounce / 100 * 0.4256) * 2 * Math.Sqrt(Stiffness(speed));

    // Spring::Step from island/src/shape_math.h sampled at 60 fps (8 sub-steps each), ending on `to`.
    public static List<double> SpringPath(double from, double to, double speed, double bounce)
    {
        double k = Stiffness(speed), c = Damping(speed, bounce), x = from, v = 0;
        var path = new List<double>();
        while (path.Count < 120 && !(Math.Abs(to - x) < 0.1 && Math.Abs(v) < 1))
        {
            for (int i = 0; i < 8; i++) { v += (k * (to - x) - c * v) / 480; x += v / 480; }
            path.Add(x);
        }
        path.Add(to);
        return path;
    }
}
