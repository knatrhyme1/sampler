# Документация

Начинайте с самого короткого полезного документа, а не читайте всё подряд.

## Продукт

| Файл | Назначение |
| --- | --- |
| [`product-status.md`](product-status.md) | Что доказано, что только в симуляции, что мешает дальше |
| [`roadmap.md`](roadmap.md) | Куда идёт продукт и в каком порядке |
| [`decision-log.md`](decision-log.md) | Принятые архитектурные решения и почему |
| [`backlog.md`](backlog.md) | Активная работа |
| [`project-rules.md`](project-rules.md) | Постоянные правила проекта |

## Пользователю

| Файл | Назначение |
| --- | --- |
| [`user-manual.md`](user-manual.md) | Как пользоваться устройством |
| [`ui-reference.md`](ui-reference.md) | Карта экранов и контролов |
| [`assets/screens/`](assets/screens/) | Снимки экранов |

## Железо

| Файл | Назначение |
| --- | --- |
| [`hardware/architecture.md`](hardware/architecture.md) | Как устроено физическое устройство |
| [`hardware/bom.md`](hardware/bom.md) | Компоненты и стадии закупки |
| [`hardware/pin-map.md`](hardware/pin-map.md) | Карта выводов |
| [`hardware/bring-up.md`](hardware/bring-up.md) | Порядок проверки собранного устройства |
| [`mpk-mapping.md`](mpk-mapping.md) | Снятое поведение внешнего контроллера |

## Разработчику

| Файл | Назначение |
| --- | --- |
| [`architecture.md`](architecture.md) | Как устроено выполнение прошивки |
| [`verification.md`](verification.md) | Что и где на самом деле проверено |
| [`known-issues.md`](known-issues.md) | Что сломано, медленно или не проверено |
| [`test-plan.md`](test-plan.md) | Сплошная проверка функционала |
| [`code-review-brief.md`](code-review-brief.md) | Чего ждать от код-ревью |
| [`archive/`](archive/) | Исторические материалы, не текущие спецификации |

## Порядок чтения

Для первого знакомства:

```text
README
  ↓
product-status
  ↓
architecture
  ↓
known-issues
```

`hardware/` — когда работаете с железом или готовите закупку.
`mpk-mapping.md` — когда работаете с вводом.
`user-manual.md` — когда нужно понять поведение устройства глазами пользователя.
