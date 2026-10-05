Please document this in Readme.md as well.

Can we add effort level for the tasks and while the task is executing, we can show partial results in the interacting window? We should not allow multi-agent mode to run when effort is below max. If an effort level is below max, we will toggle off create agent skill and not allow the user to toggle on. Otherwise, it's complimentary based on the user action to toggle on the multi-agent skill.

Here's how the effor level works:

Low: we let the model run and wait for a respond from the model, if the model respond a final answer, the harness will return whatever has happened with the model.

Medium: we let the model run and wait for a respond from the model, if the model respond a final answer, the harness will send a "can you continue? respond with a json {\"continue\":"yes/no"}" we will always keep all enabled tooling in the context window, we will also keep the past conversation (up to a limit of 100k tokens) in the context and ask the model if we can continue. If the model continue, then we will continue doing the work. Until the model responds, we will continue the processing. User can cancel at any time to stop sending message to the connector.

High: we let the model run and wait for a respond from the model, if the model respond a final answer, the harness will send a "can you continue? respond with a json {\"continue\":"yes/no"}" we will summarize the past interaction with the model for each 100k tokens and include the summarization (up to 100k) in the context and ask the model if we can continue. If the model continue, we will continue doing the work. Until the model responds, we will continue processing. User can cancel at any time to stop sending message to the connector.

Extra high: we let the model run and wait for a respond from the model, if the model respond a final answer, the harness will send a "can you continue? respond with a json {\"continue\":"yes/no"}" we will summarize the past interaction with the model for each 100k tokens and include the summarization (up to 200k) in the context and ask the model if we can continue. If the model continue, we will continue doing the work. Until the model responds, we will continue processing. User can cancel at any time to stop sending message to the connector.


Max: in this case, we will enable the model to use the create_agent skill. we let the model run and wait for a respond from the model, if the model respond a final answer and all the created agents have been stopped, the harness will send a "can you continue? respond with a json {\"continue\":"yes/no"}" we will summarize the past interaction with the model for each 100k tokens and include the summarization (up to 250k) in the context and ask the model if we can continue. If the model continue, we will continue doing the work. Until the model responds, we will continue processing. User can cancel at any time to stop sending message to the connector. And if the model stop respond, we will have another try to summarize the summarization and ask the model whether we should continue, if we get a no for continue, we will stop immediately. Otherwise, we will continue from the summarization of summarization and continue the execution with the model. 


At any stage, we should be able to cancel any agent at any time when the user click on the stop button which is overlaying the submit button.