import { IHistoryServer } from "cfx/common/services/servers/types";
import { RichEvent } from "cfx/utils/types";

export namespace MpMenuEvents {
  export const connectTo = RichEvent.define<{
    hostnameStr: string,
    connectParams: string,
  }>('connectTo');

  export const backfillServerInfo = RichEvent.define<{
    data: {
      nonce: string,
      server: Pick<IHistoryServer, 'icon' | 'token' | 'vars'>,
    }
  }>('backfillServerInfo');

  export const connecting = RichEvent.define('connecting');

  export const connectStatus = RichEvent.define<{
    data: {
      message: string,
      count: number,
      total: number,
      cancelable: boolean,
    },
  }>('connectStatus');

  export const connectFailed = RichEvent.define<{
    message: string,
    extra?:
      | { fault: 'you', status?: true, action: string }
      | { fault: 'cfx', status: true, action: string }
      | { fault: 'server', status?: true, action: string }
      | { fault: 'either', status: true, action: string },
  }>('connectFailed');
}