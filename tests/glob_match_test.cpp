/*
    SPDX-FileCopyrightText: 2026 machadex

    SPDX-License-Identifier: GPL-2.0-or-later

    Versioned matcher guarantees (run in CI, see .github/workflows/prebuilts.yml
    job "matcher-tests"):

      1. the shipped globMatch (effect/globmatch_p.h — literally the same
         header the compositor compiles) agrees with the frozen v0.6.1
         regex-translation reference on tens of thousands of randomized
         pattern/string pairs within the caps;
      2. targeted regression corpus ('??', '?*', literal dots, case-folding
         pairs, wildcards inside text);
      3. the step budgets are ceilings, not behavior: legitimate input
         never consumes them, and adversarial input stops inside them.

    Exit code != 0 on any divergence. Deterministic (fixed seeds).
*/

#include <QRegularExpression>
#include <QString>

#include "globmatch_p.h"

#include <cstdio>
#include <random>
#include <vector>

// ---------------------------------------------------------------------------
// Frozen reference: the translation shipped through v0.6.1 (regex-based).
// Must NOT be kept "in sync" by editing — it is the historical behavior the
// live matcher is verified against.
// ---------------------------------------------------------------------------
static QRegularExpression globToMatcher(const QString &glob)
{
    QString regex;
    regex.reserve(glob.size() + 8);
    int literalStart = 0;
    for (int i = 0; i < glob.size(); ++i) {
        const QChar c = glob.at(i);
        if (c != QLatin1Char('*') && c != QLatin1Char('?')) {
            continue;
        }
        if (i > literalStart) {
            regex += QRegularExpression::escape(glob.mid(literalStart, i - literalStart));
        }
        regex += (c == QLatin1Char('*')) ? QStringLiteral(".*") : QStringLiteral(".");
        literalStart = i + 1;
    }
    if (literalStart < glob.size()) {
        regex += QRegularExpression::escape(glob.mid(literalStart));
    }
    return QRegularExpression(QRegularExpression::anchoredPattern(regex),
                              QRegularExpression::CaseInsensitiveOption);
}

static bool referenceMatch(const QString &glob, const QString &text)
{
    return globToMatcher(glob).match(text).hasMatch();
}

// ---------------------------------------------------------------------------

static int s_total = 0;
static int s_mismatch = 0;

static void check(const QString &glob, const QString &text)
{
    ++s_total;
    const bool want = referenceMatch(glob, text);
    int budget = kResolveStepBudget;
    const bool got = globMatch(glob, text, &budget);
    if (want != got) {
        if (s_mismatch < 20) {
            fprintf(stderr, "MISMATCH glob=[%s] text=[%s] ref=%d live=%d\n",
                    glob.toUtf8().constData(), text.toUtf8().constData(), int(want), int(got));
        }
        ++s_mismatch;
    }
}

int main()
{
    // ---- 1. randomized equivalence fuzz, within caps ----
    {
        std::mt19937 rng(0xBADC0FFE);
        static const QChar alphabet[] = {
            u'a', u'b', u'A', u'B', u'*', u'?', u'.', u'0',
            u'é', u'É', u'Σ', u'σ', u'ς', u'ı', u'İ', u'ß', u'ẞ', u'ſ', u'x',
        };
        std::uniform_int_distribution<int> pick(0, std::size(alphabet) - 1);
        std::uniform_int_distribution<int> lenG(0, kMaxPatternLength);
        std::uniform_int_distribution<int> lenT(0, 64); // strings; long texts below
        for (int i = 0; i < 50000; ++i) {
            QString g, t;
            const int ng = lenG(rng), nt = lenT(rng);
            for (int k = 0; k < ng; ++k) g += alphabet[pick(rng)];
            for (int k = 0; k < nt; ++k) t += alphabet[pick(rng)];
            check(g, t);
        }
        // long-text sweep at the identity cap (worst realistic length)
        std::uniform_int_distribution<int> lenL(kMaxWindowIdentityLength - 40, kMaxWindowIdentityLength);
        for (int i = 0; i < 2000; ++i) {
            QString g, t;
            const int ng = 8 + int(rng() % 40);
            const int nt = lenL(rng);
            for (int k = 0; k < ng; ++k) g += alphabet[pick(rng)];
            for (int k = 0; k < nt; ++k) t += alphabet[pick(rng)];
            check(g, t);
        }
    }

    // ---- 2. targeted regression corpus ----
    {
        static const char *pairs[] = {
            // glob, text, expect ("1"/"0") — flat list
            "??bar", "XYbar", "1", "??bar", "bar", "0",
            "x.?y", "x.Ay", "1", "x.?y", "x.y", "0",
            "*?", "a", "1", "*?", "", "0",
            "?*", "ab", "1", "?*", "", "0",
            "**", "anything", "1", "a**b", "axxb", "1", "a**b", "ab", "1",
            "*fire*fox*", "afirebfoxc", "1", "org.*?dolphin", "org.xdolphin", "1",
            "org.*?dolphin", "org.dolphin", "0",
            "Fire*", "FIREFOX", "1", "?.?", "ab", "0", "?.?", "aXb", "0", "?.?", "a.b", "1",
            "σ", "ς", "1", "Σ", "ς", "1", "İ", "i", "0", "ſ", "S", "1",
            "ß", "ẞ", "1", "*.LOG", "a.B.log", "1",
            "v2.*", "v2.x", "1", "**?**", "x", "1", "**?**", "", "0",
            "app", "app", "1", "app:", "app:", "1", // ':' literal
        };
        for (size_t i = 0; i + 2 < std::size(pairs); i += 3) {
            const QString g = QString::fromUtf8(pairs[i]);
            const QString t = QString::fromUtf8(pairs[i + 1]);
            const bool want = QString(pairs[i + 2]) == QLatin1String("1");
            const bool got = globMatch(g, t);
            ++s_total;
            if (got != want) {
                fprintf(stderr, "TARGETED-FAIL glob=[%s] text=[%s] want=%d got=%d\n",
                        g.toUtf8().constData(), t.toUtf8().constData(), int(want), int(got));
                ++s_mismatch;
            }
            // and it must still agree with the frozen reference
            check(g, t);
        }
    }

    // ---- 3. budgets are ceilings, not behavior ----
    {
        // Realistic worst legitimate-ish pass: 64 rules of '*a' against a
        // full-cap identity must finish with a large budget margin.
        // rule-count cap mirrored from the effect (keep in sync with
        // kMaxAppRules there; a mismatch only weakens this assertion).
        constexpr int kRuleCapLocal = 64;
        int budget = kResolveStepBudget;
        const QString text(kMaxWindowIdentityLength, QLatin1Char('b'));
        int matches = 0;
        for (int i = 0; i < kRuleCapLocal; ++i) {
            matches += globMatch(QStringLiteral("*a"), text, &budget) ? 1 : 0;
        }
        if (matches != 0 || budget < kResolveStepBudget / 2) {
            fprintf(stderr, "BUDGET-FAIL realistic pass consumed too much (%d left, %d matches)\n",
                    budget, matches);
            ++s_mismatch;
        }
        // TRUE worst cost for the greedy matcher: a '*' followed by a long
        // literal run that only fails at its last char, against text made of
        // that same char. Every star rewind re-scans the literal suffix, so
        // cost is text x suffix, not a single pass.
        QString evil = QStringLiteral("*") + QString(126, QLatin1Char('a')) + QLatin1Char('b');
        const QString evilText(1024, QLatin1Char('a')); // never ends in 'b'
        if (referenceMatch(evil, evilText) != false || globMatch(evil, evilText) != false) {
            fprintf(stderr, "WORSTCASE-FAIL expected no-match\n");
            ++s_mismatch;
        }
        const int b2 = kResolveStepBudget;
        int b2rem = b2;
        globMatch(evil, evilText, &b2rem);
        const int consumed = b2 - b2rem;
        // must actually exercise the cost (thousands of re-scans...), stay
        // under the per-call ceiling, and refuse when the ceiling hits.
        if (consumed < 30000 || consumed > kGlobStepBudget + 1) {
            fprintf(stderr, "BUDGET-FAIL worst-case consumed %d (want 30000..%d)\n",
                    consumed, kGlobStepBudget + 1);
            ++s_mismatch;
        }

        // Exhaust a small shared budget: the call must refuse, report the
        // budget as spent (<=0), and later rules sharing it must refuse
        // immediately instead of restarting expensive scans.
        int b3 = 100;
        const bool r3 = globMatch(evil, evilText, &b3);
        if (r3 || b3 > 0) {
            fprintf(stderr, "BUDGET-FAIL tiny aggregate: result=%d budget=%d (want false, <=0)\n",
                    int(r3), b3);
            ++s_mismatch;
        }
        const int b3before = b3;
        const bool r4 = globMatch(evil, evilText, &b3); // same exhausted budget
        if (r4 || b3before - b3 > 2) {
            fprintf(stderr, "BUDGET-FAIL exhausted budget still worked (%d steps)\n", b3before - b3);
            ++s_mismatch;
        }

        // Aggregate emulation of a resolve pass: 64 worst-case rules (x two
        // ids = 128 walks) sharing one kResolveStepBudget must stop exactly
        // at the ceiling: each walk obeys the per-call cap, the shared one
        // refuses anything past it, final budget cannot go below -1.
        int agg = kResolveStepBudget;
        for (int i = 0; i < kMaxResolveWalks; ++i) {
            globMatch(evil, evilText, &agg);
        }
        // Full walks consume until the shared budget is gone; every later
        // walk refuses at its first step (one decrement). Worst total
        // overshoot is therefore bounded by the number of walks.
        if (agg < -kMaxResolveWalks || agg > kResolveStepBudget - 30000) {
            fprintf(stderr, "BUDGET-FAIL aggregate out of sane band (%d left)\n", agg);
            ++s_mismatch;
        }
        // and one walk more is refused nearly instantly
        const int before = agg;
        const bool exhaustedResult = globMatch(evil, evilText, &agg);
        if (exhaustedResult || before - agg > 1) {
            // post-exhaustion walks: false, at most the one decrement of
            // the loop's first check
            fprintf(stderr, "BUDGET-FAIL post-exhaust walk cost %d (result %d)\n",
                    before - agg, int(exhaustedResult));
            ++s_mismatch;
        }
    }

    fprintf(stderr, "glob_match_test: total=%d mismatches=%d\n", s_total, s_mismatch);
    return s_mismatch ? 1 : 0;
}
