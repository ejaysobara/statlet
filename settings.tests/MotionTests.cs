using Xunit;

namespace StatletSettings.Tests;

public class MotionTests
{
    [Fact]
    public void Defaults_give_the_designed_spring()
    {
        Assert.Equal(420, Motion.Stiffness(60), 6);
        Assert.Equal(34, Motion.Damping(60, 40), 0);
    }

    // tests/fixtures/spring.json is also checked against the island's SpringFromFeel, so the two copies cannot drift apart.
    [Fact]
    public void Matches_the_shared_spring_fixtures()
    {
        using var doc = System.Text.Json.JsonDocument.Parse(File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "fixtures", "spring.json")));
        var cases = doc.RootElement.GetProperty("cases").EnumerateArray().ToList();
        Assert.True(cases.Count >= 10);
        foreach (var c in cases)
        {
            double speed = c.GetProperty("speed").GetDouble(), bounce = c.GetProperty("bounce").GetDouble();
            Assert.Equal(c.GetProperty("stiffness").GetDouble(), Motion.Stiffness(speed), 2);  // the fixture is rounded to 4 places
            Assert.Equal(c.GetProperty("damping").GetDouble(), Motion.Damping(speed, bounce), 2);
        }
    }

    [Fact]
    public void Speed_doubles_stiffness_every_40_steps() => Assert.Equal(2 * Motion.Stiffness(20), Motion.Stiffness(60), 6);

    [Fact]
    public void No_bounce_is_critically_damped_and_full_bounce_is_not()
    {
        double k = Motion.Stiffness(60);
        Assert.Equal(2 * Math.Sqrt(k), Motion.Damping(60, 0), 6);
        Assert.Equal(0.5744 * 2 * Math.Sqrt(k), Motion.Damping(60, 100), 3);
    }

    [Fact]
    public void SpringPath_ends_on_the_target_and_stays_within_two_seconds()
    {
        foreach (double speed in new[] { 0.0, 60, 100 })
            foreach (double bounce in new[] { 0.0, 40, 100 })
            {
                var path = Motion.SpringPath(0, 100, speed, bounce);
                Assert.Equal(100, path[^1]);
                Assert.InRange(path.Count, 2, 121);
            }
    }

    [Fact]
    public void A_bouncy_spring_overshoots_and_a_critically_damped_one_does_not()
    {
        Assert.True(Motion.SpringPath(0, 100, 60, 100).Max() > 100);
        Assert.True(Motion.SpringPath(0, 100, 60, 0).Max() <= 100.001);
    }
}
