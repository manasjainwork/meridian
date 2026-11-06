# Literature gap table: latency-aware multi-agent RL exchange simulator

As of 2026-11-06. Every claim below is a hypothesis or a reading of an abstract or paper, not a result from Meridian.

## Summary

None of the papers read so far combines all three of these: exchange latency as a controlled experimental variable, several learning agents at once, and a real open-source matching engine with measured (not assumed) latency. This is a candidate gap, not a proven one. Sixteen pages were opened out of about 30 links, and no systematic search has been run, so "first" is not a claim that can be made yet.

- **Latency in simulators already exists.** [ABIDES](https://arxiv.org/abs/1904.12066) supports configurable pairwise network latency, so "we model latency" alone is not novel.
- **Latency plus RL exists for one agent.** [Relaver](https://arxiv.org/html/2505.12465v1) trains a single market-making agent under random delays.
- **Multiple learners exist without latency.** [Wang, Ventre and Polukarov](https://arxiv.org/pdf/2510.25929) train competing market makers, and the abstract describes no latency.
- **The economics of speed is settled theory.** [Budish, Cramton and Shim](https://econpapers.repec.org/RePEc:oup:qjecon:v:130:y:2015:i:4:p:1547-1621) and [Wah and Wellman](https://strategicreasoning.org/wp-content/uploads/2013/02/ec38-wah.pdf) give the hypotheses to test with learning agents.

Closest competitor so far: Belcak, Calliess and Zohren (2022, simulator MAXE). Full text read on [arXiv 2008.07871](https://arxiv.org/pdf/2008.07871): C++ core, Python agents, MIT license (github.com/maxe-team/maxe). Processing delay is swept as a parameter but simulated, not measured. Its learning is a moving-average rule among 100 identical traders, not RL.

## Gap table

Read from abstracts and first pages unless noted. "Not stated" means the page opened did not say, not that it is absent.

| Paper | Year | What they did | Latency | Learning agents | Code | What it leaves open |
| --- | --- | --- | --- | --- | --- | --- |
| [ABIDES](https://arxiv.org/abs/1904.12066), Byrd et al. | 2019 | Discrete-event market simulator, NASDAQ-style messages, tens of thousands of agents | Pairwise latency matrix plus jitter model, per-agent computation delay | None described; ML trading named as future work | [GitHub](https://github.com/abides-sim/abides), BSD 3-clause | Latency is a model, not measured from a real engine |
| [SHIFT](https://arxiv.org/html/2002.11158v3), Alves et al. | 2020 | Exchange-like platform over FIX, zero-intelligence agents, crash scenario | Network latency "can be explored" | No, zero-intelligence agents | Not stated | No learning, latency not studied |
| [Karpe et al.](https://arxiv.org/abs/2006.05574) | 2020 | Execution agent trained with Double DQN inside ABIDES with six momentum agents | Inherited from ABIDES | One learner | Not stated | Latency not varied in the abstract |
| [Gao and Wang](https://arxiv.org/abs/1806.05849) | 2018 | MDP model of market making for large-tick assets under latency | Central: latency adds risk and cuts returns | No, MDP theory | Not stated | Single maker, exogenous order flow |
| [Relaver](https://arxiv.org/html/2505.12465v1), Jiang et al. | 2025 | RL market maker with order hold time, DP teacher, trend predictor | Random delay, one simulated range (abstract 30-100 ms, text 30-80 ms) | One PPO-LSTM agent, five seeds | Anonymous review link only | Single agent, replayed data, delay assumed not measured |
| [Wang, Ventre, Polukarov](https://arxiv.org/pdf/2510.25929) | 2025 | Hierarchical multi-agent RL market makers, no collusion found | Not mentioned | Yes, PPO, several agents | Not stated | No latency, exogenous price |
| [StockMARL](https://www.sciencedirect.com/science/article/pii/S1877050925038128), Zou and Siebers | 2025 | DQN learner among rule-based agents, daily bars | Low, daily steps | One learner | Not stated | No microstructure; its own table and text disagree on returns |
| [Wah and Wellman](https://strategicreasoning.org/wp-content/uploads/2013/02/ec38-wah.pdf) | 2013 | Two-market model, latency arbitrage, call markets as fix | Fixed delay to the NBBO | No, ZI agents and a rule arbitrageur | Not stated | Fixed strategies, no learning |
| [Budish, Cramton, Shim](https://econpapers.repec.org/RePEc:oup:qjecon:v:130:y:2015:i:4:p:1547-1621) | 2015 | Arms race is a market-design flaw; frequent batch auctions | Speed arbitrage is the subject | No, empirical and theory | Not stated | Abstract only, full text not opened |
| [Belcak, Calliess, Zohren (MAXE)](https://arxiv.org/pdf/2008.07871) | 2022 | Message-driven C++ simulator with Python agents, pro-rata matching study | Processing delay swept, simulated | Moving-average rule, 100 identical traders, no RL | MIT, github.com/maxe-team/maxe | No RL, delay not measured |
| [Chakraborty and Kearns](https://www.cis.upenn.edu/~mkearns/papers/marketmaking.pdf) | 2011 | Simple market-making rule profits on mean-reverting prices | Assumes zero latency | No | Not stated | Single maker, exogenous prices |
| [Law and Viens](https://arxiv.org/pdf/1903.07222) | 2020 | Market making with switching and impulse control on point processes | Not a focus | No, numerical PDE | Not stated | No learning, no interaction |
| [Muhle-Karbe et al.](https://arxiv.org/pdf/2601.23172) | 2026 | Two-layer Hawkes order-flow model, power-law impact | No | No | Not stated | Order-flow model only |
| [Jain et al., Compound Hawkes](https://www.sciencedirect.com/science/article/pii/S1544612324011863) | 2024 | Hawkes LOB simulator with order sizes, concave impact | Not discussed | No | Not stated | Aggregate flow, no agents |
| [Varosyan and Cliff, DSXE](https://research-information.bris.ac.uk/en/publications/a-cloud-native-distributed-real-time-multi-agent-stock-exchange-s/) | 2024 | Cloud-deployable C++ multi-agent exchange for inter-market arbitrage | Throughput measured, 355 msg/s, no latency figures | Not mentioned | Stated to be on GitHub, no link seen | Closest on engineering, no RL, no latency study |
| [Kanagal et al.](https://web.stanford.edu/class/msande448/2017/Final/Reports/gr4.pdf) | 2017 | SVM and forest signals to reprice a market maker | Not a focus | No, supervised | Said to be on GitHub, no link | Course report, weak evidence |

Added from the SSRN abstracts:

- [Jain, Algorithmic Predatory Equilibrium](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=6087766) (2026): multi-agent RL against human traders in decentralized finance, with equilibrium claims. Latency not mentioned. Several learners. Code not stated. Open: DeFi setting and theory, no exchange engine.
- [Park et al.](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=5517658) (2025): closed-form optimal quotes when trading intensity and volatility jump. No latency, no learners.
- SSRN 6155066 is the same paper as Muhle-Karbe et al. above.

## Found in the Scholar search

- [JaxMARL-HFT](https://arxiv.org/html/2511.02136v1) (Mohl et al., 2025), opened: GPU, open-source ([code](https://github.com/vmohl/JaxMARL-HFT)), independent PPO with two learners (a market maker and an execution agent). Latency is not modeled; steps process messages in a fixed order. It says ABIDES-gym trains a single RL agent only.
- ABIDES-MARL (Cheridito, Dupret, Wu, arXiv 2511.02016), abstract only: several learning agents on top of ABIDES-Gym, optimal execution. No latency in the abstract.
- Gupta et al. (2025), adaptive jitter in ABIDES, abstract only: replaces ABIDES's fixed and cubic-jitter latency models with a feedback model; rule-based agents, no RL.
- Ramcharitar (June 2026), working paper: proposes a fill-probability model plus a latency-aware RL quoting agent for retail traders with 200 ms to 2 s delays. A framework and research agenda, no experiments, and the delay is the trader's own, not the exchange's.
- CoinTossX (Jericevich, Sing, Gebbie): matching engine modeled on JSE rules, 86% Java, MIT license, Aeron messaging, Hawkes client simulation, HdrHistogram latency output. Last release 5 years old, no RL interface in the README. Closest engineering comparison to Meridian, and not C++.
- Snippet only, to open later: ABIDES-gym (Amrouni et al., 2021), PyMarketSim (Mascioli et al., 2024), JAX-LOB (Frey et al., 2023), mbt-gym, Patel 2018, Cho 2026 thesis (Southampton).

## Full texts read

- **Relaver.** One learning agent against rule-based baselines, five seeds, replayed data from four Chinese index contracts. Latency is one simulated range, not swept. The agent quotes into a replayed market, so no second learner reacts to it. The paper mixes up "Mean Absolute Position" and "Mean Absolute Percentage" for MAP, so quote its numbers carefully.
- **ABIDES.** Python, BSD 3-clause. Latency is a pairwise matrix plus noise model, with a per-agent computation delay; single-threaded and deterministic for a given seed. Case studies use rule-based agents, no RL. The paper lists a co-location experiment as a future use, so H2 below is well motivated but not a new idea.
- **MAXE (Belcak et al.).** See the summary above.

## Sources set aside

- Could not open: Semantic Scholar and ResearchGate pages, IEEE 7163720. The Budish PDF on the UMD site blocks automated access, so only its abstract was used.
- Not research papers, not used: a Reddit thread, two Medium posts, DeepWiki and AlphaXiv mirrors of ABIDES, a DLT settlement article, the BIS market-making report, the OFR CDS paper.
- Still to search: other latency plus RL papers after 2023, and any open-source C++ exchange built for RL. Until that is done, "first" is not a claim to make.

## Hypotheses and month-1 steps

These are hypotheses to test, each taken from a paper above. None is a result.

1. **H1, latency hurts learners.** As exchange latency rises, a learned market maker's inventory risk rises and its return falls (Gao and Wang predict it; Relaver reports it for one agent). Test with several learners at once.
2. **H2, latency changes who wins.** With several learning agents, the faster agent takes a larger share of fills, as in Wah and Wellman's arbitrageur. Test whether learned agents find this on their own.
3. **H3, batching removes the effect.** A batch auction every 100 ms shrinks the speed advantage, as Budish et al. argue. Test it with learning agents, which neither paper did.
4. **H4, assumed delay misleads.** A delay measured on the Meridian engine differs from a fixed random delay in conclusions. Only Meridian can test this.

Month-1 steps:

- [ ] Run the remaining searches and add rows.
- [ ] Write hypotheses, metrics and the crash definition into one file in the repo.
- [ ] Read the full text of Wang et al. and ABIDES-MARL before any novelty claim.
