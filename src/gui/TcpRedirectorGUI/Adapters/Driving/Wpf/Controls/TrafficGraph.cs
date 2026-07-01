using System.Collections.Specialized;
using System.Windows;
using System.Windows.Media;

namespace TcpRedirectorGUI.Adapters.Driving.Wpf.Controls;

/// <summary>
/// Lightweight WPF control that renders a real-time RX/TX traffic chart
/// using DrawingContext (no external charting library required).
/// Subscribes to INotifyCollectionChanged to auto-refresh when points are added.
/// </summary>
public class TrafficGraph : FrameworkElement
{
    public TrafficGraph()
    {
        ClipToBounds = true;
    }

    // ── Data ──────────────────────────────────────────
    public static readonly DependencyProperty RxDataProperty =
        DependencyProperty.Register(nameof(RxData), typeof(IList<TrafficPoint>),
            typeof(TrafficGraph), new FrameworkPropertyMetadata(null,
                FrameworkPropertyMetadataOptions.AffectsRender, OnDataChanged));

    public static readonly DependencyProperty TxDataProperty =
        DependencyProperty.Register(nameof(TxData), typeof(IList<TrafficPoint>),
            typeof(TrafficGraph), new FrameworkPropertyMetadata(null,
                FrameworkPropertyMetadataOptions.AffectsRender, OnDataChanged));

    public IList<TrafficPoint>? RxData
    {
        get => (IList<TrafficPoint>?)GetValue(RxDataProperty);
        set => SetValue(RxDataProperty, value);
    }

    public IList<TrafficPoint>? TxData
    {
        get => (IList<TrafficPoint>?)GetValue(TxDataProperty);
        set => SetValue(TxDataProperty, value);
    }

    /// <summary>Graph time window in seconds (default 3600 = 1 hour).</summary>
    public static readonly DependencyProperty WindowSecondsProperty =
        DependencyProperty.Register(nameof(WindowSeconds), typeof(double),
            typeof(TrafficGraph), new FrameworkPropertyMetadata(3600.0,
                FrameworkPropertyMetadataOptions.AffectsRender));

    public double WindowSeconds
    {
        get => (double)GetValue(WindowSecondsProperty);
        set => SetValue(WindowSecondsProperty, value);
    }

    /// <summary>
    /// When RxData/TxData changes, subscribe to CollectionChanged
    /// so InvalidateVisual is called automatically on every add/remove.
    /// </summary>
    private static void OnDataChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        var ctrl = (TrafficGraph)d;
        if (e.OldValue is INotifyCollectionChanged oldColl)
            oldColl.CollectionChanged -= ctrl.OnCollectionChanged;
        if (e.NewValue is INotifyCollectionChanged newColl)
            newColl.CollectionChanged += ctrl.OnCollectionChanged;
        // Force an initial render pass
        ctrl.InvalidateVisual();
    }

    private void OnCollectionChanged(object? sender, NotifyCollectionChangedEventArgs e)
    {
        InvalidateVisual();
    }

    /// <summary>
    /// Ensure the element gets non-zero layout space.
    /// </summary>
    protected override Size MeasureOverride(Size availableSize)
    {
        // If parent provides infinite size, use a minimum
        var w = double.IsFinite(availableSize.Width) ? availableSize.Width : 200;
        var h = double.IsFinite(availableSize.Height) ? availableSize.Height : 100;
        return new Size(w, h);
    }

    // ── Drawing ───────────────────────────────────────
    protected override void OnRender(DrawingContext dc)
    {
        base.OnRender(dc);
        var w = ActualWidth;
        var h = ActualHeight;

        if (w <= 0 || h <= 0) return;

        // Background
        dc.DrawRectangle(new SolidColorBrush(Color.FromRgb(0x1E, 0x1E, 0x1E)), null, new Rect(0, 0, w, h));

        // Border
        var borderPen = new Pen(new SolidColorBrush(Color.FromRgb(0x3C, 0x3C, 0x3C)), 1);
        dc.DrawRectangle(null, borderPen, new Rect(0, 0, w, h));

        // Grid lines (horizontal — 4 lines)
        var gridPen = new Pen(new SolidColorBrush(Color.FromRgb(0x2D, 0x2D, 0x2D)), 0.5);
        for (int i = 1; i <= 3; i++)
        {
            var y = h * i / 4;
            dc.DrawLine(gridPen, new Point(0, y), new Point(w, y));
        }

        // Axis labels
        var labelBrush = new SolidColorBrush(Color.FromRgb(0x85, 0x85, 0x85));
        var typeface = new Typeface("Segoe UI");
        var fontSize = 10;

        var maxVal = ComputeMax(RxData, TxData, (long)(WindowSeconds * 1000));
        if (maxVal <= 0) maxVal = 1;

        // Y-axis labels
        for (int i = 0; i <= 4; i++)
        {
            var val = maxVal * (ulong)(4 - i) / 4;
            var label = FormatByteRate(val);
            var ft = new FormattedText(label, System.Globalization.CultureInfo.CurrentCulture,
                FlowDirection.LeftToRight, typeface, fontSize, labelBrush, 96);
            dc.DrawText(ft, new Point(4, h * i / 4 - ft.Height / 2));
        }

        // X-axis labels
        var winSec = (int)WindowSeconds;
        if (winSec < 10) winSec = 10;
        var step = winSec / 4;
        for (int i = 0; i <= 4; i++)
        {
            var sec = i * step;
            var label = FormatTimeLabel(winSec - sec);
            var ft = new FormattedText(label, System.Globalization.CultureInfo.CurrentCulture,
                FlowDirection.LeftToRight, typeface, fontSize, labelBrush, 96);
            var xPos = 4 + sec * (w - 50) / winSec;
            dc.DrawText(ft, new Point(xPos - ft.Width / 2, h - ft.Height - 2));
        }

        // Draw data lines
        var margin = 45.0; // left margin for Y labels
        var plotW = w - margin - 8;
        var plotH = h - 20;

        DrawLine(dc, RxData, maxVal, plotW, plotH, margin, (int)WindowSeconds * 1000L, Color.FromRgb(0x4E, 0xC9, 0xB0)); // green
        DrawLine(dc, TxData, maxVal, plotW, plotH, margin, (int)WindowSeconds * 1000L, Color.FromRgb(0xCE, 0x91, 0x78)); // orange

        // Legend
        DrawLegend(dc, w);
    }

    private static void DrawLine(DrawingContext dc, IList<TrafficPoint>? data,
        ulong maxVal, double plotW, double plotH, double margin, long windowMs, Color color)
    {
        if (data == null || data.Count < 2 || maxVal == 0) return;

        var pen = new Pen(new SolidColorBrush(color), 1.5);
        var points = new List<Point>();

        // Normalize time: last windowMs milliseconds
        var now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        var tStart = now - windowMs;

        foreach (var pt in data)
        {
            if (pt.Timestamp < tStart) continue;
            var x = margin + plotW * (pt.Timestamp - tStart) / (double)windowMs;
            var y = plotH - plotH * pt.BytesPerSec / (double)maxVal;
            points.Add(new Point(x, y));
        }

        if (points.Count < 2) return;

        for (int i = 1; i < points.Count; i++)
            dc.DrawLine(pen, points[i - 1], points[i]);
    }

    private static void DrawLegend(DrawingContext dc, double w)
    {
        var typeface = new Typeface("Segoe UI");
        var labelBrush = new SolidColorBrush(Color.FromRgb(0xCC, 0xCC, 0xCC));

        var rxFt = new FormattedText("RX", System.Globalization.CultureInfo.CurrentCulture,
            FlowDirection.LeftToRight, typeface, 11, new SolidColorBrush(Color.FromRgb(0x4E, 0xC9, 0xB0)), 96);
        var txFt = new FormattedText("TX", System.Globalization.CultureInfo.CurrentCulture,
            FlowDirection.LeftToRight, typeface, 11, new SolidColorBrush(Color.FromRgb(0xCE, 0x91, 0x78)), 96);

        dc.DrawText(rxFt, new Point(w - 80, 4));
        dc.DrawText(txFt, new Point(w - 40, 4));
    }

    private static ulong ComputeMax(IList<TrafficPoint>? rx, IList<TrafficPoint>? tx, long windowMs)
    {
        var now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        var tStart = now - windowMs;

        // Collect all visible non-zero values for percentile calculation
        var vals = new List<ulong>();
        if (rx != null)
            foreach (var p in rx)
                if (p.Timestamp >= tStart && p.BytesPerSec > 0)
                    vals.Add(p.BytesPerSec);
        if (tx != null)
            foreach (var p in tx)
                if (p.Timestamp >= tStart && p.BytesPerSec > 0)
                    vals.Add(p.BytesPerSec);

        if (vals.Count == 0) return 1024;

        vals.Sort();
        ulong actualMax = vals[^1];

        // Use 95th percentile so outlier spikes don't flatten the Y-axis for hours.
        // Floor at actualMax / 10 so huge spikes are still somewhat visible.
        int p95idx = (int)(vals.Count * 0.95);
        if (p95idx >= vals.Count) p95idx = vals.Count - 1;
        ulong p95 = vals[p95idx];

        ulong result = Math.Max(p95, actualMax / 10);
        if (result == 0) result = 1024;

        // Round up to nice number
        ulong mag = 1;
        ulong r = result;
        while (r >= 1000) { r /= 10; mag *= 10; }
        return (r + 1) * mag;
    }

    private static string FormatByteRate(ulong bps)
    {
        if (bps >= 1_000_000) return $"{bps / 1_000_000.0:F1} MB/s";
        if (bps >= 1_000) return $"{bps / 1_000.0:F0} KB/s";
        return $"{bps} B/s";
    }

    private static string FormatTimeLabel(int totalSec)
    {
        if (totalSec >= 3600 && totalSec % 3600 == 0)
            return $"-{totalSec / 3600}h";
        if (totalSec >= 60 && totalSec % 60 == 0)
            return $"-{totalSec / 60}m";
        return $"-{totalSec}s";
    }
}

/// <summary>
/// Data point for the traffic chart.
/// </summary>
/// <param name="Timestamp">Unix milliseconds</param>
/// <param name="BytesPerSec">Bytes per second at this point</param>
public record struct TrafficPoint(long Timestamp, ulong BytesPerSec);