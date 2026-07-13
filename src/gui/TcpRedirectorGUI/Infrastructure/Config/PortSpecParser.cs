using System.Text;
using TcpRedirectorGUI.Domain.Entities;

namespace TcpRedirectorGUI.Infrastructure.Config;

// ===========================================================================
// WP5 — Multi-port / port-range parser.
//
// Grammar (comma-separated, whitespace ignored):
//     spec  := (token (',' token)*)?
//     token := PORT | PORT '-' PORT
//     PORT  := integer in [1..65535]
//
// Contract:
//   - Empty / whitespace-only input                → success, empty lists.
//   - Any invalid token                            → returns false, non-empty error.
//   - ports[] is deduplicated + sorted ascending.
//   - port_ranges[] preserves user order (no auto-merge), but any range
//     with from > to is rejected. Overlaps are OK — matches C++ WP3.
//   - Never throws. Suitable for INotifyDataErrorInfo hot-path calls.
//
// Canonical output of Format():
//     "port, port, ..., from-to, from-to, ..."
// with individual ports first (ascending) and ranges after (ascending by From).
// ===========================================================================

/// <summary>
/// Parses and formats textual port specifications like <c>"80, 443, 8000-8100"</c>
/// into their <see cref="PortRange"/> + <see cref="int"/> list forms. Static;
/// no exceptions on invalid input.
/// </summary>
public static class PortSpecParser
{
    /// <summary>
    /// Parses <paramref name="input"/> into discrete <paramref name="ports"/>
    /// and <paramref name="ranges"/>. Returns false on parse failure with a
    /// human-readable <paramref name="error"/>. Empty input succeeds with
    /// empty lists.
    /// </summary>
    public static bool TryParse(
        string? input,
        out List<int> ports,
        out List<PortRange> ranges,
        out string error)
    {
        ports = new List<int>();
        ranges = new List<PortRange>();
        error = "";

        if (string.IsNullOrWhiteSpace(input))
            return true; // empty is allowed → both lists empty

        var seenPorts = new HashSet<int>();
        // Split on comma; whitespace is trimmed per-token.
        var tokens = input.Split(',');
        foreach (var raw in tokens)
        {
            var token = raw.Trim();
            if (token.Length == 0)
            {
                // "80,,443" — two adjacent commas. Reject as malformed.
                error = "Пустой элемент в списке портов";
                return false;
            }

            var dash = token.IndexOf('-');
            if (dash >= 0)
            {
                // Range form: "from-to". dash==0 (e.g. "-80") is invalid.
                if (dash == 0 || dash == token.Length - 1)
                {
                    error = $"Некорректный диапазон: '{token}'";
                    return false;
                }
                var fromStr = token[..dash].Trim();
                var toStr = token[(dash + 1)..].Trim();
                if (!TryParsePort(fromStr, out var from) ||
                    !TryParsePort(toStr, out var to))
                {
                    error = $"Некорректный диапазон: '{token}'";
                    return false;
                }
                if (from > to)
                {
                    error = $"Некорректный диапазон: '{token}' (начало > конец)";
                    return false;
                }
                ranges.Add(new PortRange { From = from, To = to });
            }
            else
            {
                if (!TryParsePort(token, out var p))
                {
                    error = $"Некорректный порт: '{token}'";
                    return false;
                }
                if (seenPorts.Add(p))
                    ports.Add(p);
            }
        }

        // Canonical output for discrete ports: ascending sorted (dedup already done).
        ports.Sort();
        return true;
    }

    /// <summary>
    /// Renders <paramref name="ports"/> and <paramref name="ranges"/> in a
    /// canonical form: ascending discrete ports first, then ascending ranges,
    /// separated by <c>", "</c>. Suitable for round-tripping through
    /// <see cref="TryParse"/>.
    /// </summary>
    public static string Format(IEnumerable<int> ports, IEnumerable<PortRange> ranges)
    {
        var sb = new StringBuilder();
        var first = true;

        foreach (var p in ports.OrderBy(x => x))
        {
            if (!first) sb.Append(", ");
            sb.Append(p);
            first = false;
        }

        foreach (var r in ranges.OrderBy(x => x.From).ThenBy(x => x.To))
        {
            if (!first) sb.Append(", ");
            sb.Append(r.From);
            sb.Append('-');
            sb.Append(r.To);
            first = false;
        }

        return sb.ToString();
    }

    /// <summary>Integer in [1..65535]. No leading + / whitespace / hex.</summary>
    private static bool TryParsePort(string s, out int value)
    {
        value = 0;
        if (string.IsNullOrEmpty(s)) return false;
        // Reject anything that isn't a pure decimal digit sequence — int.TryParse
        // would otherwise silently accept "+80", " 80 ", etc. after our trim
        // (trim is already done by caller); we still want to reject "+80" to
        // stay strict with the grammar.
        foreach (var c in s)
        {
            if (c < '0' || c > '9') return false;
        }
        if (!int.TryParse(s, out var v)) return false;
        if (v < 1 || v > 65535) return false;
        value = v;
        return true;
    }
}
