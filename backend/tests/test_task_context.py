"""Effort window, public synthesis, pairing and continuation protocol regressions."""
import asyncio
import copy
import json
import unittest

from backend.app.services.task_context import (
    EffortContext, TaskContextError, ContinuationError, estimate_tokens,
    normalize_effort, parse_continue, CONTINUE_PROMPT, CONTINUE_WORK_PROMPT,
)

TOOLS = [{'type': 'function', 'function': {'name': 'python', 'description': 'Execute Python',
         'parameters': {'type': 'object', 'properties': {'code': {'type': 'string'}}}}}]


class SummaryConnector:
    def __init__(self, reply='Completed public work; preserve constraints and continue outstanding tasks.'):
        self.requests = []
        self.reply = reply

    async def turn(self, url, model, messages, tools=None, validate_model=True):
        self.requests.append({'url': url, 'model': model, 'messages': copy.deepcopy(messages),
                              'tools': tools, 'validate_model': validate_model})
        if isinstance(self.reply, Exception):
            raise self.reply
        return {'text': self.reply, 'tool_calls': [], 'reasoning_content': 'Private fixture detail'}


def initial(history=0, text_size=180):
    return [{'role': 'system', 'text': 'Preserve selected project and enabled tools.'},
            {'role': 'developer', 'text': 'Publish progress only.'},
            *[{'role': 'assistant', 'text': f'Historical result {index}: ' + 'x' * text_size} for index in range(history)],
            {'role': 'user', 'text': 'Finish the current project task.'}]


def tool_pair(identifier='call_python_1', result_size=60):
    return [
        {'role': 'assistant', 'content': None, 'tool_calls': [
            {'id': identifier, 'type': 'function', 'function': {'name': 'python', 'arguments': '{"code":"print(42)"}'}}]},
        {'role': 'tool', 'tool_call_id': identifier, 'content': '42 ' + 'x' * result_size},
    ]


class ContinuationProtocolTests(unittest.TestCase):
    def test_only_explicit_json_yes_and_no_are_accepted(self):
        self.assertTrue(parse_continue(' {"continue":"yes"} '))
        self.assertFalse(parse_continue('{"continue":"no"}'))
        invalid = ['', 'yes', 'null', '[]', '{"continue":true}', '{"continue":"Yes"}',
                   '{"continue":"maybe"}', '{"continue":"no","continue":"yes"}',
                   '{"continue":"yes","reasoning":"private"}', '```json\n{"continue":"no"}\n```',
                   '{"continue":NaN}', 'x' * 1025, None]
        for text in invalid:
            with self.subTest(text_type=type(text).__name__):
                with self.assertRaises(ContinuationError):
                    parse_continue(text)

    def test_estimates_are_explicitly_utf8_and_include_structural_costs(self):
        self.assertGreater(estimate_tokens('\u4e2d' * 40), estimate_tokens('a' * 40))
        self.assertGreater(estimate_tokens([{'role': 'user', 'text': 'a'}]), estimate_tokens('a'))
        self.assertEqual(normalize_effort('Extra high'), 'extra_high')
        with self.assertRaises(TaskContextError):
            normalize_effort('automatic')


class EffortContextTests(unittest.IsolatedAsyncioTestCase):
    async def test_low_has_no_continuation_or_synthesis_and_keeps_original_task(self):
        connector = SummaryConnector()
        context = EffortContext('low', initial(15), token_limit=1000)
        context.append({'role': 'assistant', 'text': 'A final answer.'})
        result = await context.prepare(connector, 'connector', 'model', TOOLS)
        self.assertFalse(context.continues)
        self.assertEqual(connector.requests, [])
        self.assertEqual([message['role'] for message in result[:2]], ['system', 'developer'])
        self.assertIn('Finish the current project task.', json.dumps(result))
        self.assertNotIn('Historical result 0:', json.dumps(result))
        self.assertEqual(result[-1]['text'], 'A final answer.')
        self.assertLessEqual(context.context_tokens(TOOLS), 1000)

    async def test_medium_can_drop_a_large_historical_answer_before_the_new_current_task(self):
        context = EffortContext('medium', initial(1, text_size=4000), token_limit=1000)
        result = await context.prepare(SummaryConnector(), 'connector', 'model', TOOLS)
        self.assertEqual([message['role'] for message in result], ['system', 'developer', 'user'])
        self.assertEqual(result[-1]['text'], 'Finish the current project task.')
        self.assertLessEqual(context.context_tokens(TOOLS), 1000)

    async def test_explicit_duplicate_current_task_pins_its_latest_occurrence(self):
        task = {'role': 'user', 'text': 'Repeated current request.'}
        context = EffortContext('medium', [task, *initial(12), task], token_limit=1000, current_task=task)
        result = await context.prepare(SummaryConnector(), 'connector', 'model', TOOLS)
        self.assertEqual(result[-1]['text'], 'Repeated current request.')
        self.assertEqual(sum(message.get('text') == 'Repeated current request.' for message in result), 1)

    async def test_large_medium_history_is_trimmed_without_synthesis(self):
        connector = SummaryConnector()
        context = EffortContext('medium', initial(10000, text_size=20))
        result = await context.prepare(connector, 'connector', 'model', TOOLS)
        self.assertEqual(result[-1]['text'], 'Finish the current project task.')
        self.assertLess(len(result), 10000)
        self.assertLessEqual(context.context_tokens(TOOLS), context.token_limit)
        self.assertEqual(connector.requests, [])

    async def test_medium_trims_complete_tool_pairs_and_counts_enabled_tool_schemas(self):
        connector = SummaryConnector()
        context = EffortContext('medium', initial(8), token_limit=1000)
        context.extend(tool_pair())
        context.append({'role': 'assistant', 'text': 'Python completed.'})
        context.append({'role': 'user', 'text': CONTINUE_PROMPT})
        result = await context.prepare(connector, 'connector', 'model', TOOLS)
        tool_messages = [message for message in result if message.get('tool_calls') or message['role'] == 'tool']
        self.assertEqual(len(tool_messages), 2)
        self.assertEqual(tool_messages[0]['tool_calls'][0]['id'], tool_messages[1]['tool_call_id'])
        self.assertIn('Finish the current project task.', json.dumps(result))
        self.assertEqual(result[-1]['text'], CONTINUE_PROMPT)
        self.assertLessEqual(context.context_tokens(TOOLS), 1000)
        self.assertGreater(context.context_tokens(TOOLS), context.context_tokens())
        self.assertEqual(connector.requests, [])

    async def test_trimming_never_keeps_a_tool_result_after_its_call_is_dropped(self):
        context = EffortContext('medium', initial(), token_limit=800)
        context.extend(tool_pair(result_size=700))
        context.extend([{'role': 'assistant', 'text': 'New result: ' + 'n' * 350} for _ in range(6)])
        result = await context.prepare(SummaryConnector(), 'connector', 'model')
        self.assertFalse(any(message['role'] == 'tool' for message in result))
        self.assertFalse(any(message.get('tool_calls') for message in result))
        self.assertIn('Finish the current project task.', json.dumps(result))

    async def test_incomplete_and_duplicate_tool_results_are_controlled_errors(self):
        for additions in [tool_pair()[:1], tool_pair() + [tool_pair()[1]],
                          [tool_pair()[0], {'role': 'tool', 'tool_call_id': 'unrelated', 'text': 'Result'}]]:
            context = EffortContext('medium', initial(), token_limit=1000)
            context.extend(additions)
            with self.assertRaises(TaskContextError):
                await context.prepare(SummaryConnector(), 'connector', 'model')
        orphan = EffortContext('medium', [*initial(), {'role': 'tool', 'tool_call_id': 'old_missing', 'text': 'Old result'}])
        result = await orphan.prepare(SummaryConnector(), 'connector', 'model')
        self.assertFalse(any(message['role'] == 'tool' for message in result))

    async def test_high_summarizes_public_history_and_keeps_tool_instructions_and_current_task(self):
        connector = SummaryConnector()
        events = []
        context = EffortContext('high', initial(20), token_limit=1000,
                                on_summary=lambda phase, details: events.append((phase, details)))
        context.extend(tool_pair())
        result = await context.prepare(connector, 'connector', 'model', TOOLS)
        self.assertGreater(context.summary_count, 0)
        self.assertGreater(len(connector.requests), 0)
        for request in connector.requests:
            self.assertIsNone(request['tools'])
            self.assertFalse(request['validate_model'])
            self.assertLessEqual(estimate_tokens(request['messages']), 1000)
            source = json.loads(request['messages'][1]['text'])
            self.assertEqual(source['current_task']['text'], 'Finish the current project task.')
            self.assertEqual(len(source['instructions']), 2)
        self.assertIn('Public summaries of earlier task interaction', json.dumps(result))
        self.assertIn('Preserve selected project and enabled tools.', json.dumps(result))
        self.assertIn('Finish the current project task.', json.dumps(result))
        self.assertNotIn('Private fixture detail', json.dumps(result))
        self.assertLessEqual(context.context_tokens(TOOLS), 1000)
        self.assertEqual(events[0][0], 'summarizing')
        self.assertEqual(events[-1][0], 'summarized')
        self.assertEqual(events[-1][1]['summary_count'], context.summary_count)
        tool_messages = [message for message in result if message.get('tool_calls') or message['role'] == 'tool']
        self.assertIn(len(tool_messages), (0, 2))

    async def test_extra_high_and_max_have_larger_summary_windows(self):
        for effort, limit in [('high', 1000), ('extra_high', 2000), ('max', 2500)]:
            context = EffortContext(effort, initial(5), token_limit=1000)
            result = await context.prepare(SummaryConnector(), 'connector', 'model', TOOLS)
            self.assertEqual(context.window_limit, limit)
            self.assertLessEqual(context.context_tokens(TOOLS), limit)
            self.assertIn('Finish the current project task.', json.dumps(result))

    async def test_large_windows_still_compact_each_threshold_of_new_interaction(self):
        for effort in ('extra_high', 'max'):
            connector = SummaryConnector()
            context = EffortContext(effort, initial(14), token_limit=1000)
            self.assertLess(context.context_tokens(), context.window_limit)
            self.assertGreater(context.context_tokens(), context.token_limit)
            await context.prepare(connector, 'connector', 'model', TOOLS)
            self.assertGreater(context.summary_count, 0)
            self.assertLessEqual(context.context_tokens(TOOLS), context.window_limit)

    async def test_accumulated_summaries_are_compacted_instead_of_dropping_earlier_progress(self):
        connector = SummaryConnector('Earlier artifacts and validations remain complete.')
        phases = []
        async def callback(phase, details):
            phases.append((phase, details['summary_count']))
        context = EffortContext('high', initial(), token_limit=1000, on_summary=callback)
        context.summaries = [f'Public summary {index}: ' + 'x' * 900 for index in range(4)]
        result = await context.prepare(connector, 'connector', 'model', TOOLS)
        self.assertGreater(len(connector.requests), 1)
        fragments = []
        for request in connector.requests[:-1]:
            self.assertLessEqual(estimate_tokens(request['messages']), context.window_limit)
            source = json.loads(request['messages'][1]['text'])
            fragments.append(source['public_interaction'][0]['text'].split('\n', 1)[1])
        reconstructed = json.loads(''.join(fragments))
        self.assertEqual(len(reconstructed['earlier_public_summaries']), 4)
        self.assertEqual(len(context.summaries), 1)
        self.assertIn('Earlier artifacts and validations', json.dumps(result))
        self.assertEqual(phases[0], ('summarizing', 0))
        self.assertEqual(phases[-1], ('summarized', context.summary_count))

    async def test_oversized_public_tool_group_is_fragmented_without_orphan_execution_messages(self):
        connector = SummaryConnector()
        context = EffortContext('high', initial(), token_limit=1000)
        pair = tool_pair(result_size=10000)
        context.extend(pair)
        result = await context.prepare(connector, 'connector', 'model', TOOLS)
        self.assertGreater(len(connector.requests), 2)
        for request in connector.requests:
            self.assertLessEqual(estimate_tokens(request['messages']), context.window_limit)
            self.assertFalse(any(message['role'] == 'tool' for message in request['messages']))
            self.assertIsNone(request['tools'])
        self.assertFalse(any(message['role'] == 'tool' or message.get('tool_calls') for message in result))
        self.assertIn('Finish the current project task.', json.dumps(result))
        self.assertLessEqual(context.context_tokens(TOOLS), context.window_limit)

    async def test_max_recovery_uses_bounded_source_fragments_for_large_interaction(self):
        connector = SummaryConnector()
        context = EffortContext('max', initial(40), token_limit=1000)
        context.summaries = ['Prior public progress.']
        result = await context.recover(connector, 'connector', 'model', TOOLS)
        self.assertGreater(len(connector.requests), 2)
        self.assertTrue(context.recovery_used)
        for request in connector.requests:
            self.assertLessEqual(estimate_tokens(request['messages']), context.token_limit)
            source = json.loads(request['messages'][1]['text'])
            self.assertEqual(source['current_task']['text'], 'Finish the current project task.')
        self.assertIn('Public summaries of earlier task interaction', json.dumps(result))
        self.assertLessEqual(context.context_tokens(TOOLS), context.window_limit)

    async def test_max_recovery_summarizes_summaries_once_and_preserves_instructions(self):
        connector = SummaryConnector('Recovered public progress: continue remaining work.')
        context = EffortContext('max', initial(4), token_limit=1000)
        context.summaries = ['Earlier public result: created artifact.', 'Later public result: validated artifact.']
        context.append({'role': 'user', 'text': CONTINUE_PROMPT})
        result = await context.recover(connector, 'connector', 'model', TOOLS)
        self.assertTrue(context.recovery_used)
        self.assertEqual(len(connector.requests), 1)
        source = json.loads(connector.requests[0]['messages'][1]['text'])
        self.assertEqual(len(source['earlier_public_summaries']), 2)
        self.assertIn('Historical result 0:', json.dumps(source['public_interaction']))
        self.assertEqual(len(context.messages), 1)
        self.assertEqual(context.messages[0]['text'], 'Finish the current project task.')
        self.assertIn('Recovered public progress:', json.dumps(result))
        with self.assertRaisesRegex(TaskContextError, 'exhausted'):
            await context.recover(connector, 'connector', 'model', TOOLS)
        self.assertEqual(len(connector.requests), 1)
        with self.assertRaises(TaskContextError):
            await EffortContext('high', initial()).recover(connector, 'connector', 'model')

    async def test_failed_max_recovery_is_not_retried_and_does_not_erase_progress(self):
        connector = SummaryConnector('')
        context = EffortContext('max', initial(2), token_limit=1000)
        context.summaries = ['Public work remains.']
        before = copy.deepcopy(context.messages)
        with self.assertRaisesRegex(TaskContextError, 'public task progress summary'):
            await context.recover(connector, 'connector', 'model')
        self.assertEqual(context.messages, before)
        self.assertEqual(context.summaries, ['Public work remains.'])
        with self.assertRaises(TaskContextError):
            await context.recover(connector, 'connector', 'model')
        self.assertEqual(len(connector.requests), 1)

    async def test_cancelling_summary_propagates_without_further_connector_calls(self):
        started = asyncio.Event()
        class WaitingConnector(SummaryConnector):
            async def turn(self, *args, **kwargs):
                self.requests.append('started')
                started.set()
                await asyncio.Event().wait()
        connector = WaitingConnector()
        context = EffortContext('high', initial(20), token_limit=1000)
        task = asyncio.create_task(context.prepare(connector, 'connector', 'model', TOOLS))
        await asyncio.wait_for(started.wait(), timeout=1)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertEqual(connector.requests, ['started'])
        self.assertEqual(context.summary_count, 0)

    async def test_private_metadata_is_discarded_before_any_synthesis_request(self):
        messages = initial(20)
        messages[2]['reasoning_content'] = 'Private fixture input'
        messages[2]['author'] = 'Untrusted author'
        connector = SummaryConnector()
        context = EffortContext('high', messages, token_limit=1000)
        result = await context.prepare(connector, 'connector', 'model')
        self.assertNotIn('Private fixture input', json.dumps(connector.requests))
        self.assertNotIn('Private fixture detail', json.dumps(result))
        self.assertNotIn('Untrusted author', json.dumps(connector.requests))
        self.assertNotIn('reasoning_content', json.dumps(context.messages))

    async def test_oversized_pinned_task_fails_without_transport_and_summary_loops_are_bounded(self):
        connector = SummaryConnector('x' * 12000)
        context = EffortContext('medium', [{'role': 'user', 'text': 'x' * 4000}], token_limit=1000)
        with self.assertRaisesRegex(TaskContextError, 'exceed'):
            await context.prepare(connector, 'connector', 'model', TOOLS)
        self.assertEqual(connector.requests, [])
        high = EffortContext('high', [{'role': 'system', 'text': 'x' * 2000},
                                    {'role': 'user', 'text': 'x' * 600}], token_limit=1000)
        high.summaries = ['x' * 2000]
        with self.assertRaises(TaskContextError):
            await high.prepare(connector, 'connector', 'model')
        self.assertLessEqual(len(connector.requests), 1)

    async def test_original_prompt_is_verbatim_on_every_repeated_turn_and_synthesis(self):
        original = 'Complete "the original task" for \u4f60\u597d.\nKeep C:\\project\\input.json and the exact constraints.\nDo not change the scope.'
        for effort in ('medium', 'high', 'extra_high', 'max'):
            with self.subTest(effort=effort):
                messages = [*initial(15), {'role': 'user', 'text': original},
                            {'role': 'assistant', 'text': 'Older response to the same task.'},
                            {'role': 'user', 'text': original}]
                context = EffortContext(effort, messages, token_limit=1000, current_task=original)
                anchor = context.task
                messages[-1]['text'] = 'Caller mutation must not change the submitted task.'
                connector = SummaryConnector('Summary deliberately omits the original task text.')
                for stage in range(3):
                    context.append({'role': 'assistant', 'text': f'Public stage {stage}: ' + 'x' * 900})
                    context.append({'role': 'user', 'text': CONTINUE_PROMPT})
                    probe = await context.prepare(connector, 'connector', 'model', TOOLS)
                    self.assertEqual(probe[-1]['text'], CONTINUE_PROMPT)
                    await connector.turn('connector', 'model', probe, tools=TOOLS, validate_model=False)
                    context.append({'role': 'assistant', 'text': '{"continue":"yes"}'})
                    context.append({'role': 'user', 'text': CONTINUE_WORK_PROMPT})
                    context.extend(tool_pair(identifier=f'call_stage_{stage}', result_size=1100))
                    work = await context.prepare(connector, 'connector', 'model', TOOLS)
                    await connector.turn('connector', 'model', work, tools=TOOLS, validate_model=False)
                    self.assertIs(context.task, anchor)
                if effort == 'max':
                    await context.recover(connector, 'connector', 'model', TOOLS)
                    context.append({'role': 'user', 'text': CONTINUE_PROMPT})
                    probe = await context.prepare(connector, 'connector', 'model', TOOLS)
                    await connector.turn('connector', 'model', probe, tools=TOOLS, validate_model=False)
                    context.append({'role': 'assistant', 'text': '{"continue":"yes"}'})
                    context.append({'role': 'user', 'text': CONTINUE_WORK_PROMPT})
                    work = await context.prepare(connector, 'connector', 'model', TOOLS)
                    await connector.turn('connector', 'model', work, tools=TOOLS, validate_model=False)
                for request in connector.requests:
                    if request['tools'] is None:
                        source = json.loads(request['messages'][1]['text'])
                        self.assertEqual(source['current_task']['text'], original)
                    else:
                        self.assertEqual(request['tools'], TOOLS)
                        self.assertTrue(any(message['role'] == 'user' and message.get('text') == original
                                            for message in request['messages']))
                self.assertEqual(anchor['text'], original)
                if effort == 'medium':
                    self.assertEqual(context.summary_count, 0)
                else:
                    self.assertGreater(context.summary_count, 0)

    async def test_missing_or_mutated_original_pin_fails_instead_of_sending_a_different_task(self):
        original = 'Keep the original user request.'
        for mutation in ('missing', 'changed'):
            context = EffortContext('high', [{'role': 'user', 'text': original},
                                            {'role': 'assistant', 'text': 'Historical duplicate.'},
                                            {'role': 'user', 'text': original}], current_task=original)
            if mutation == 'missing':
                context.messages = [message for message in context.messages if message is not context.task]
            else:
                context.task['text'] = 'Changed request.'
            connector = SummaryConnector()
            with self.assertRaisesRegex(TaskContextError, 'original user prompt'):
                await context.prepare(connector, 'connector', 'model', TOOLS)
            self.assertEqual(connector.requests, [])

    async def test_original_prompt_that_cannot_fit_is_never_silently_summarized_or_truncated(self):
        original = 'Original constraint: ' + 'x' * 10000
        for effort in ('medium', 'high', 'extra_high', 'max'):
            context = EffortContext(effort, [*initial(10), {'role': 'user', 'text': original}],
                                    token_limit=1000, current_task=original)
            connector = SummaryConnector()
            with self.assertRaises(TaskContextError):
                await context.prepare(connector, 'connector', 'model', TOOLS)
            self.assertEqual(connector.requests, [])
            self.assertEqual(context.task['text'], original)

    async def test_summary_model_cannot_request_executable_tools(self):
        class CallingConnector(SummaryConnector):
            async def turn(self, *args, **kwargs):
                return {'text': 'Attempt to execute', 'tool_calls': [{'id': 'no_execution'}]}
        context = EffortContext('high', initial(20), token_limit=1000)
        with self.assertRaisesRegex(TaskContextError, 'public task progress summary'):
            await context.prepare(CallingConnector(), 'connector', 'model')


if __name__ == '__main__':
    unittest.main()
